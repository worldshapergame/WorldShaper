#include "world/world_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "core/hash.hpp"
#include "core/jobs.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "world/brick.hpp"
#include "world/ledger.hpp"
#include "world/property.hpp"
#include "world/tags.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

constexpr u32 kMagic = 0x57534357u;   // "WSCW"
// 2 — the sharpened-region list, so a world cached before it finished can be carried on rather
// than mistaken for a finished one. An older file is rejected by the header check and rebuilt.
// 3 — R9g: a chunk's emissive cells are written beside the world.
// 4 — the stipple verdict, without which a resumed world cannot despeckle anything it sharpens.
// Not read compatibly from a version 3 file: the missing verdict is exactly the fault being fixed,
// so a file that does not have one is rebuilt rather than loaded and quietly left speckled.
// 5 — the ladder's whole leaf set, each leaf with its octree key and the detail it was sampled at,
// so a resuming run rebuilds the tree rather than trying to recognise it by containment. Again not
// read compatibly: a version 4 file holds only the finest boxes and no keys, which is the fault
// being fixed, and a file that cannot say what its coarse leaves are is worth less than a rebuild.
// 6 — R11f: a mode byte in the header and an edit-only payload, so a world can be written as its
// clip plus what somebody changed rather than as sixty million voxels. This one could not have
// been read compatibly whatever anybody wanted: a version 5 file has no mode byte, so every field
// after the key sits at a different offset, and a reader that guessed at the mode would not fail —
// it would misread a whole world as a difference, or a difference as a whole world.
// 7 — R11j: the file is a JOURNAL, so a save after a save writes only what changed since it. Not
// read compatibly, and this one could not be: version 6 has its payload where version 7 has a
// fixed-size header, so an old file read as a new one would take the tag count for a journal
// length. An old file is refused by the version check and the world is rebuilt, which is what the
// version field has always been for here.
// 8 — R12d: WHO wrote a brick. A bit in each brick's tag byte says a person's work is in it, and
// each chunk carries the list of brick slots a person EMPTIED — the carve, which has no brick left
// to hold a flag. Both are provenance and neither is a voxel, so a version 7 file decodes to
// exactly the same world; what it cannot say is which of that world is derivable, and a reader
// that guessed would guess "none of it", which turns every carving back into base world on the
// first reload. That is the failure R12d exists to prevent, so it is a version bump and not a
// tolerated absence. **Bumped rather than smuggled in**: the tag bit alone would have been
// invisible to a version 7 reader in the worst way — 0x80 | 2 is not a tag it knows, so it would
// refuse the brick and lose the chunk — and the erased list changes the framing of a chunk record,
// which an old reader would walk straight past the end of. The key already carries `build_stamp`,
// so a file from another build never loads anyway; the version is what says so honestly.
constexpr u32 kVersion = 8u;

// ---------------------------------------------------------------------------------------
// R11j — the shape of the file
//
//   [ header, 64 bytes ][ journal ]
//
// and the header is the commit record: the file is committed to exactly `64 + journal_bytes`
// bytes, and that is what the reader reads. Fewer than that is a truncated file and is refused;
// MORE is the tail of an append that did not finish, and it is ignored, because the header never
// claimed it. The journal is a run of segments laid end to end:
//
//   [ Full ][ Directory ]                                  a first write
//   [ Full ][ Directory ][ Increment ][ Directory ] ...     and every save after it
//
// A `Full` segment is a whole world. An `Increment` says what changed since everything before it:
// the chunks whose bytes moved, the blocks of the region list that moved, and the metadata if any
// of it did. A `Directory` is the writer's own note to the next writer — a hash per chunk, a hash
// per block of region list, one for the metadata — so the next save can work out what to append
// without reading a word of the payload. The reader skips it.
//
// Replaying the journal gives the world the last save had, which is the whole promise: a file
// written incrementally reads back as the same world as one written whole.
// ---------------------------------------------------------------------------------------

constexpr u32 kSegmentMagic = 0x53435357u;   // "WSCS"
constexpr usize kHeaderBytes = 64;
constexpr usize kSegmentHeaderBytes = 16;

// THERE IS NO "A WRITE IS IN PROGRESS" FLAG, AND THERE MUST NOT BE ONE.
//
// The first version of this carried one, on the reasoning that D701 wants an interrupted write
// told apart from a finished one. Both halves of that were wrong.
//
// D701's `running`/`done` is the fourth field of the BAKE STAMP — a separate file that
// `tools/bake_world.ps1` writes after every pass — and the question it answers is whether the bake
// LOOP finished, which is a fact about how complete the WORLD is. A bake stopped between passes
// leaves a complete file holding a partial world, and `-RequireWhole` uses the stamp to refuse to
// publish it. Nothing about that lives in this header, and nothing in `tools/` reads this header.
//
// The other half is worse. A flag saying "somebody is writing" has to go down BEFORE the write and
// come up after it, so for the length of the append the file says "refuse me" — and what it is
// refusing is the perfectly good world sitting underneath, which no reader can now reach. That
// turns a crash during a tenth of a second into losing the whole cache, which is exactly the
// failure the two-minute bank exists to prevent.
//
// So the commit record is the HEADER ITSELF, written last. `journal_bytes` says how long the
// journal is and the check hash covers it, so the file is committed to precisely
// `kHeaderBytes + journal_bytes` bytes. A crash part way through an append leaves bytes past that
// point which the header never claimed, and a reader that stops at the committed end is not being
// lenient — it is reading exactly what was committed. See the append below and `read_world_cache`.

constexpr u32 kSegmentFull = 0u;
constexpr u32 kSegmentIncrement = 1u;
constexpr u32 kSegmentDirectory = 2u;

// How many leaves of the ladder share one hash in the directory.
//
// The region list is the one piece of metadata that is not small — the estate's is six hundred
// thousand leaves and eighty-one bytes each — and it changes on every save by construction, since
// a save happens BECAUSE nodes were sharpened. Restating it whole would put fifty megabytes in
// every increment and leave the bank exactly as expensive as it was.
//
// What makes a block work is that the leaf list is stable by index: the ladder replaces the node
// it split with its first child and appends the other seven, so a sharpened node moves the block it
// sits in and the block at the end, and nothing in between.
//
// SIXTY-FOUR, and the size is chosen against the case that actually happens rather than against one
// sharpened node. A bank is two minutes of ladder, which is hundreds of nodes picked by rank —
// scattered right through the list, not clustered. So the cost is roughly "one block per node
// refined", and the block size is what that multiplies: at a thousand leaves a block, a bank that
// refined a thousand nodes would restate the estate's whole fifty-megabyte leaf list; at
// sixty-four it restates about five kilobytes each and stops well short of it. What small blocks
// cost is the directory — one hash per block, an eighth of a byte per leaf — and sixteen bytes of
// framing per block in the increment, both of which are nothing.
constexpr u32 kRegionsPerBlock = 64u;

// When the journal is rewritten whole rather than appended to.
//
// Without a bound the file creeps: a chunk rewritten fifty times is in the file fifty times, and
// the dead copies are never read but are always carried. Rewriting once the journal has doubled
// makes the file at most about twice the world and makes the amortised cost of a save the cost of
// what changed — the rewrite is paid once per doubling, not once per save.
constexpr u32 kMaxSegments = 4096u;

// A brick, exactly as it is held. No canonical form, no re-encode: the whole reason this file
// exists is that it can be read faster than the world can be rebuilt, and a normalisation pass
// over sixty million voxels is most of what it is trying to avoid.
//
// R12d rides in the top bit of the tag byte. A brick's form is three values and will never be more
// than a handful, so the byte had seven spare bits and the flag costs nothing — no second list, no
// second pass, and a brick that is written at all carries who wrote it. Version 8; see kVersion
// for why an old reader must not be allowed to try.
constexpr u8 kBrickEditedBit = 0x80u;

void write_brick_raw(std::vector<u8>& out, const Brick& brick) {
    const auto put_u32 = [&out](u32 value) {
        out.push_back(static_cast<u8>(value));
        out.push_back(static_cast<u8>(value >> 8));
        out.push_back(static_cast<u8>(value >> 16));
        out.push_back(static_cast<u8>(value >> 24));
    };
    const u8 edited = (edit_tracking() && brick.edited()) ? kBrickEditedBit : u8{0};

    if (brick.uniform()) {
        out.push_back(static_cast<u8>(0u | edited));
        put_u32(brick.uniform_value());
        return;
    }

    // A palette when the brick has one, because most bricks in most worlds hold a handful of
    // materials and writing five hundred and twelve full type ids for four distinct values is
    // four times the file for no gain. The clip-forge's own worlds are the opposite case — every
    // voxel its own record — and those fall through to the flat form below, which is the fastest
    // thing to read and, at 512 distinct types, also the smallest.
    if (brick.form() != Brick::Form::Direct) {
        const std::vector<VoxelTypeId>& palette = brick.palette_data();
        const std::vector<u8>& indices = brick.index_data();
        out.push_back(static_cast<u8>(2u | edited));
        out.push_back(static_cast<u8>(brick.index_bits()));
        put_u32(static_cast<u32>(palette.size()));
        for (VoxelTypeId type : palette) put_u32(type);
        out.insert(out.end(), indices.begin(), indices.end());
        return;
    }

    VoxelTypeId decoded[kBrickVoxels];
    brick.decode(decoded);
    out.push_back(static_cast<u8>(1u | edited));
    const usize at = out.size();
    out.resize(at + sizeof(decoded));
    std::memcpy(out.data() + at, decoded, sizeof(decoded));
}

// Whether the brick written at `data` is a person's work. The caller needs this BEFORE it asks the
// chunk for somewhere to put the brick, because the chunk is what keeps the count and the only way
// in is `brick_for_write`'s origin — see Brick::set_edited on why not the other door.
bool brick_was_edited(const u8* data) { return (data[0] & kBrickEditedBit) != 0; }

// How many bytes the brick written at `data` occupies, without decoding it. Used to find where
// one chunk's payload ends so the chunks can then be filled in parallel.
usize brick_span(const u8* data, usize available) {
    if (available < 1) return 0;
    // The top bit is the R12d flag and says nothing about the length; the form is the rest.
    const u8 tag = static_cast<u8>(data[0] & ~kBrickEditedBit);
    if (tag == 0u) return (available >= 5) ? 5 : 0;
    if (tag == 1u) {
        const usize span = 1 + kBrickVoxels * sizeof(VoxelTypeId);
        return (available >= span) ? span : 0;
    }
    if (tag != 2u || available < 6) return 0;
    const u32 bits = data[1];
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8) return 0;
    u32 palette_size = 0;
    std::memcpy(&palette_size, data + 2, sizeof(palette_size));
    const usize span = 6 + palette_size * sizeof(VoxelTypeId) +
                       (static_cast<usize>(kBrickVoxels) * bits + 7) / 8;
    return (available >= span) ? span : 0;
}

usize read_brick_raw(const u8* data, usize available, Brick& brick) {
    const usize span = brick_span(data, available);
    if (span == 0) return 0;
    const u8 tag = static_cast<u8>(data[0] & ~kBrickEditedBit);

    if (tag == 0u) {
        u32 value = 0;
        std::memcpy(&value, data + 1, sizeof(value));
        brick.fill(value);
        return span;
    }

    VoxelTypeId decoded[kBrickVoxels];
    if (tag == 1u) {
        std::memcpy(decoded, data + 1, sizeof(decoded));
        brick.assign(decoded);
        return span;
    }

    const u32 bits = data[1];
    u32 palette_size = 0;
    std::memcpy(&palette_size, data + 2, sizeof(palette_size));
    const u8* palette = data + 6;
    const u8* indices = palette + palette_size * sizeof(VoxelTypeId);
    const u32 per_byte = 8u / bits;
    const u32 mask = (1u << bits) - 1;
    for (u32 base = 0; base < static_cast<u32>(kBrickVoxels); base += per_byte) {
        const u8 packed = indices[base / per_byte];
        for (u32 i = 0; i < per_byte; ++i) {
            const u32 slot = (packed >> (i * bits)) & mask;
            if (slot >= palette_size) return 0;
            std::memcpy(&decoded[base + i], palette + slot * sizeof(VoxelTypeId),
                        sizeof(VoxelTypeId));
        }
    }
    brick.assign(decoded);
    return span;
}

template <typename T>
void put_pod(std::vector<u8>& out, const T& value) {
    const usize at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

struct Cursor {
    const u8* data = nullptr;
    usize size = 0;
    usize at = 0;
    bool ok = true;

    template <typename T>
    T pod() {
        T value{};
        if (at + sizeof(T) > size) {
            ok = false;
            return value;
        }
        std::memcpy(&value, data + at, sizeof(T));
        at += sizeof(T);
        return value;
    }

    const u8* take(usize bytes) {
        if (at + bytes > size) {
            ok = false;
            return nullptr;
        }
        const u8* p = data + at;
        at += bytes;
        return p;
    }

    // Could there be `count` things of `each` bytes left? Asked before any of the reserves below,
    // because a count is a number out of a file and `reserve` believes it: a corrupt four-byte
    // field is a request for sixteen gigabytes, and `bad allocation` from inside a cache read is a
    // crash rather than the cache miss it ought to be.
    bool room_for(u64 count, usize each) const {
        return at <= size && count <= static_cast<u64>((size - at) / each);
    }
};

// Every part of a payload is preceded by its own length, so a reader that wants to SKIP a part —
// which is most of what replaying a journal is — does not have to be able to parse it. Without
// that, finding out where the metadata ends means interning every tag in it, and the reader would
// intern the same tags once per segment to reach the chunks.
usize open_part(std::vector<u8>& out) {
    const usize at = out.size();
    put_pod(out, static_cast<u64>(0));
    return at;
}

void close_part(std::vector<u8>& out, usize at) {
    const u64 bytes = static_cast<u64>(out.size() - at - sizeof(u64));
    std::memcpy(out.data() + at, &bytes, sizeof(bytes));
}

// The bytes of one part, and where it is. Zero length means "this segment does not restate it",
// which is not the same as an empty one — every part written here has at least a count in it.
const u8* take_part(Cursor& in, usize& bytes) {
    const u64 length = in.pod<u64>();
    if (!in.ok) return nullptr;
    const u8* at = in.take(static_cast<usize>(length));
    if (!in.ok) return nullptr;
    bytes = static_cast<usize>(length);
    return at;
}

// --------------------------------------------------------------------------------------
// R11j: the header, and what makes a torn one detectable
// --------------------------------------------------------------------------------------

struct CacheHeader {
    u64 key = 0;
    u8 mode = 0;
    u32 segments = 0;
    u64 journal_bytes = 0;
    u64 directory_at = 0;   // where the live directory segment starts, absolute
    u64 base_bytes = 0;     // what the journal was immediately after the last whole write
    u64 generation = 0;     // saves so far, for the log
};

// A hash over every field, so a header half-written by a machine going to sleep is not read as a
// header. Sixty-four bytes is one sector on every disk anybody runs this on and a torn write of it
// is not supposed to be possible — but "not supposed to be possible" is how a cache ends up
// pointing a reader at a journal length that was never written, and the cure is eight bytes.
// A hash over a run of bytes, eight at a time.
//
// `core/hash.hpp`'s `hash_bytes` is FNV-1a and reads one byte per multiply, which is the right
// shape for a mod name and the wrong one here: this is asked of every chunk of the world on every
// save, and at a byte a multiply the estate's three hundred megabytes would cost more than the
// write it is deciding about. Eight bytes a step through the same mixing function the rest of this
// repository uses. It is not compared against anything outside this file, so it is free to change
// whenever the version does.
u64 hash_span(const u8* data, usize size) {
    u64 value = hash_mix(static_cast<u64>(size));
    usize at = 0;
    for (; at + sizeof(u64) <= size; at += sizeof(u64)) {
        u64 word = 0;
        std::memcpy(&word, data + at, sizeof(word));
        value = hash_combine(value, word);
    }
    if (at < size) {
        u64 word = 0;
        std::memcpy(&word, data + at, size - at);
        value = hash_combine(value, word);
    }
    return value;
}

u64 header_check(const CacheHeader& head) {
    u64 value = hash_mix(kMagic);
    value = hash_combine(value, kVersion);
    value = hash_combine(value, head.key);
    value = hash_combine(value, static_cast<u64>(head.mode));
    value = hash_combine(value, static_cast<u64>(head.segments));
    value = hash_combine(value, head.journal_bytes);
    value = hash_combine(value, head.directory_at);
    value = hash_combine(value, head.base_bytes);
    value = hash_combine(value, head.generation);
    return value;
}

void encode_header(u8 out[kHeaderBytes], const CacheHeader& head) {
    std::memset(out, 0, kHeaderBytes);
    const u32 magic = kMagic;
    const u32 version = kVersion;
    const u64 check = header_check(head);
    std::memcpy(out + 0, &magic, sizeof(magic));
    std::memcpy(out + 4, &version, sizeof(version));
    std::memcpy(out + 8, &head.key, sizeof(head.key));
    out[16] = head.mode;
    std::memcpy(out + 20, &head.segments, sizeof(head.segments));
    std::memcpy(out + 24, &head.journal_bytes, sizeof(head.journal_bytes));
    std::memcpy(out + 32, &head.directory_at, sizeof(head.directory_at));
    std::memcpy(out + 40, &head.base_bytes, sizeof(head.base_bytes));
    std::memcpy(out + 48, &head.generation, sizeof(head.generation));
    std::memcpy(out + 56, &check, sizeof(check));
}

bool decode_header(const u8* data, usize size, CacheHeader& out) {
    if (data == nullptr || size < kHeaderBytes) return false;
    u32 magic = 0, version = 0;
    std::memcpy(&magic, data + 0, sizeof(magic));
    std::memcpy(&version, data + 4, sizeof(version));
    if (magic != kMagic || version != kVersion) return false;
    std::memcpy(&out.key, data + 8, sizeof(out.key));
    out.mode = data[16];
    std::memcpy(&out.segments, data + 20, sizeof(out.segments));
    std::memcpy(&out.journal_bytes, data + 24, sizeof(out.journal_bytes));
    std::memcpy(&out.directory_at, data + 32, sizeof(out.directory_at));
    std::memcpy(&out.base_bytes, data + 40, sizeof(out.base_bytes));
    std::memcpy(&out.generation, data + 48, sizeof(out.generation));
    u64 check = 0;
    std::memcpy(&check, data + 56, sizeof(check));
    if (check != header_check(out)) return false;
    if (out.mode != static_cast<u8>(WorldCacheMode::Whole) &&
        out.mode != static_cast<u8>(WorldCacheMode::EditOnly)) {
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------------------
// R11j: the directory — what the next writer needs and the reader does not
// --------------------------------------------------------------------------------------

struct DirectoryChunk {
    i64 x = 0, y = 0, z = 0;
    u64 hash = 0;
};

struct CacheDirectory {
    std::vector<DirectoryChunk> chunks;
    u32 regions = 0;
    std::vector<u64> region_blocks;
    u64 meta_hash = 0;
};

void write_directory(std::vector<u8>& out, const CacheDirectory& dir) {
    put_pod(out, static_cast<u32>(dir.chunks.size()));
    for (const DirectoryChunk& chunk : dir.chunks) {
        put_pod(out, chunk.x);
        put_pod(out, chunk.y);
        put_pod(out, chunk.z);
        put_pod(out, chunk.hash);
    }
    put_pod(out, dir.regions);
    put_pod(out, static_cast<u32>(dir.region_blocks.size()));
    for (u64 hash : dir.region_blocks) put_pod(out, hash);
    put_pod(out, dir.meta_hash);
}

bool read_directory(Cursor& in, CacheDirectory& out) {
    const u32 chunks = in.pod<u32>();
    if (!in.ok || !in.room_for(chunks, 32)) return false;
    out.chunks.resize(chunks);
    for (u32 i = 0; i < chunks && in.ok; ++i) {
        out.chunks[i].x = in.pod<i64>();
        out.chunks[i].y = in.pod<i64>();
        out.chunks[i].z = in.pod<i64>();
        out.chunks[i].hash = in.pod<u64>();
    }
    if (!in.ok) return false;
    out.regions = in.pod<u32>();
    const u32 blocks = in.pod<u32>();
    if (!in.ok || !in.room_for(blocks, sizeof(u64))) return false;
    out.region_blocks.resize(blocks);
    for (u32 i = 0; i < blocks && in.ok; ++i) out.region_blocks[i] = in.pod<u64>();
    out.meta_hash = in.pod<u64>();
    return in.ok;
}

// --------------------------------------------------------------------------------------
// The region list, by block
// --------------------------------------------------------------------------------------

u64 hash_region(u64 value, const CachedRegion& region) {
    for (i64 v : region.key) value = hash_combine(value, static_cast<u64>(v));
    value = hash_combine(value, static_cast<u64>(region.level));
    for (f64 v : region.low) {
        u64 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        value = hash_combine(value, bits);
    }
    for (f64 v : region.high) {
        u64 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        value = hash_combine(value, bits);
    }
    value = hash_combine(value, static_cast<u64>(static_cast<u32>(region.applied_per_metre)));
    value = hash_combine(value, region.done ? 1ull : 0ull);
    return value;
}

u32 region_blocks_for(u32 count) { return (count + kRegionsPerBlock - 1) / kRegionsPerBlock; }

std::vector<u64> region_block_hashes(const std::vector<CachedRegion>& regions) {
    const u32 count = static_cast<u32>(regions.size());
    std::vector<u64> out(region_blocks_for(count), 0);
    for (u32 block = 0; block < static_cast<u32>(out.size()); ++block) {
        const u32 from = block * kRegionsPerBlock;
        const u32 to = std::min<u32>(from + kRegionsPerBlock, count);
        u64 value = hash_mix(to - from);
        for (u32 i = from; i < to; ++i) value = hash_region(value, regions[i]);
        out[block] = value;
    }
    return out;
}

void write_region(std::vector<u8>& out, const CachedRegion& region) {
    for (i64 v : region.key) put_pod(out, v);
    put_pod(out, region.level);
    for (f64 v : region.low) put_pod(out, v);
    for (f64 v : region.high) put_pod(out, v);
    put_pod(out, region.applied_per_metre);
    put_pod(out, static_cast<u8>(region.done ? 1u : 0u));
}

bool read_region(Cursor& in, CachedRegion& region) {
    for (i64& v : region.key) v = in.pod<i64>();
    region.level = in.pod<u32>();
    for (f64& v : region.low) v = in.pod<f64>();
    for (f64& v : region.high) v = in.pod<f64>();
    region.applied_per_metre = in.pod<i32>();
    region.done = in.pod<u8>() != 0u;
    return in.ok;
}

// --------------------------------------------------------------------------------------
// R11f: what "the same as what the clip builds" means, brick by brick.
// --------------------------------------------------------------------------------------

// Do these two bricks hold the same voxels?
//
// `Brick::operator==` already answers this and answers it correctly — contents, never encoding —
// but it answers it by reading all five hundred and twelve voxels through `get()`, and a diff over
// the facility asks the question a quarter of a million times about bricks that are nearly always
// equal, which is the worst case that comparison has: no early exit and a form test per voxel.
//
// So: identical encodings are compared as bytes, which they are for every brick either world got
// from the same sampler, and anything else falls through to the honest comparison. The fast path
// can only ever say "equal" about bricks that really are, because two bricks with the same form,
// the same palette in the same order and the same indices decode to the same voxels by
// construction. A permuted palette simply misses the fast path and is compared properly.
bool bricks_identical(const Brick& a, const Brick& b) {
    if (a.uniform() && b.uniform()) return a.uniform_value() == b.uniform_value();
    if (a.form() == b.form() && a.palette_data() == b.palette_data() &&
        a.index_data() == b.index_data() && a.direct_data() == b.direct_data()) {
        return true;
    }
    return a == b;
}

// Do a named edit box and a box of voxels overlap? Both inclusive of their corners.
bool box_touches(const CachedEditBox& box, const i64 low[3], const i64 high[3]) {
    for (u32 axis = 0; axis < 3; ++axis) {
        if (box.high[axis] < low[axis] || box.low[axis] > high[axis]) return false;
    }
    return true;
}

// The order `World::sorted_chunk_coords` puts them in, so two sorted lists can be merged.
bool chunk_coord_less(const ChunkCoord& a, const ChunkCoord& b) {
    if (a.z != b.z) return a.z < b.z;
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
}

// A chunk holding something, as against one that exists and does not. The distinction is the
// whole of D620: an allocated chunk with no bricks in it claims matter the world does not have.
bool chunk_is_live(const Chunk* chunk) { return chunk != nullptr && !chunk->empty(); }
bool brick_is_live(const Brick* brick) { return brick != nullptr && !brick->empty(); }

// Every voxel of a world put through a table of "the id I had -> the id this file means".
//
// Data-loss case 2, and it is the whole building rather than one brick of it. See the block at
// the reader's type table below for why the two tables can differ at all; this is what is done
// about it. Only ever called when they DO differ, which is why it can afford to be a walk of the
// world: in the ordinary case the reading run interned in the same order as the writing one, the
// map is the identity, and none of this runs.
void remap_world_types(World& world, const std::vector<VoxelTypeId>& remap, JobSystem* jobs) {
    const std::vector<ChunkCoord> coords = world.sorted_chunk_coords();
    std::vector<Chunk*> chunks;
    chunks.reserve(coords.size());
    // On this thread: the world's map is not safe to insert into from several. Every one of these
    // already exists, so nothing is created here.
    for (const ChunkCoord& coord : coords) chunks.push_back(&world.chunk_for_write(coord));

    const auto work = [&](usize from, usize to) {
        VoxelTypeId decoded[kBrickVoxels];
        const u32 axis = static_cast<u32>(kChunkBricks);
        for (usize i = from; i < to; ++i) {
            Chunk& chunk = *chunks[i];
            for (u32 bz = 0; bz < axis; ++bz) {
                for (u32 by = 0; by < axis; ++by) {
                    for (u32 bx = 0; bx < axis; ++bx) {
                        const Brick* brick = chunk.brick(bx, by, bz);
                        if (brick == nullptr) continue;
                        if (brick->uniform()) {
                            const VoxelTypeId was = brick->uniform_value();
                            const VoxelTypeId now = (was < remap.size()) ? remap[was] : was;
                            if (now != was) chunk.brick_for_write(bx, by, bz).fill(now);
                            continue;
                        }
                        brick->decode(decoded);
                        bool changed = false;
                        for (u32 v = 0; v < static_cast<u32>(kBrickVoxels); ++v) {
                            const VoxelTypeId was = decoded[v];
                            const VoxelTypeId now = (was < remap.size()) ? remap[was] : was;
                            if (now == was) continue;
                            decoded[v] = now;
                            changed = true;
                        }
                        if (changed) chunk.brick_for_write(bx, by, bz).assign(decoded);
                    }
                }
            }
            chunk.mark_modified();
        }
    };
    if (jobs != nullptr && chunks.size() > 1) {
        jobs->parallel_for(chunks.size(), 1, work);
    } else {
        work(0, chunks.size());
    }
}

// Every chunk of a world that holds something, in the canonical order.
std::vector<ChunkCoord> live_chunk_coords(const World& world) {
    std::vector<ChunkCoord> out = world.sorted_chunk_coords();
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&world](const ChunkCoord& c) {
                                 return !chunk_is_live(world.chunk(c));
                             }),
              out.end());
    return out;
}

constexpr u16 brick_slot(u32 bx, u32 by, u32 bz) {
    return static_cast<u16>((bx << 10) | (by << 5) | bz);
}

// Is `inner` wholly inside `outer`? Both inclusive of their corners.
bool box_contains(const CachedEditBox& outer, const CachedEditBox& inner) {
    for (u32 axis = 0; axis < 3; ++axis) {
        if (inner.low[axis] < outer.low[axis] || inner.high[axis] > outer.high[axis]) return false;
    }
    return true;
}

// One chunk index of a voxel coordinate, clamped so an op at the far end of a 64-bit world does
// not overflow the range walk below.
i64 chunk_index_of(i64 voxel) { return chunk_of(voxel); }

// --------------------------------------------------------------------------------------
// The metadata block: everything about a world that is not its voxels
// --------------------------------------------------------------------------------------

// Written in ONE canonical order, and that is not a tidiness rule.
//
// Two of these lists come out of hash maps — the lamps out of `World::for_each_chunk`, the ledger
// out of `MatterLedger::totals` — and a hash map hands them over in whatever order it likes, which
// differs between two runs over the same world. Left alone, that makes the metadata block hash
// differently every launch, so a save that changed NOTHING still restates it, and the file grows a
// segment every time somebody opens the world. Sorting costs a few hundred comparisons and turns
// "nothing changed" into a write of nought bytes.
void write_meta_block(std::vector<u8>& out, const WorldCache& cache) {
    // Tags and properties, by name, so a build that registers them in a different order is
    // rejected by the reader rather than silently mismatched.
    put_pod(out, static_cast<u32>(cache.tags->count()));
    for (u32 i = 0; i < cache.tags->count(); ++i) {
        const std::string name(cache.tags->name(i));
        put_pod(out, static_cast<u32>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
    }
    put_pod(out, static_cast<u32>(cache.properties->count()));
    for (u32 i = 0; i < cache.properties->count(); ++i) {
        const PropertyInfo& info = cache.properties->info(i);
        put_pod(out, static_cast<u32>(info.name.size()));
        out.insert(out.end(), info.name.begin(), info.name.end());
        put_pod(out, static_cast<u8>(info.type));
        put_pod(out, static_cast<u8>(info.domain));
        put_pod(out, info.default_value.bits);
    }

    // The type table, as three arrays. Visual records are fixed-size and go out in one block;
    // behaviour records carry variable-length tag overflow and properties, and there are a
    // handful of them, so they are written one at a time.
    const std::vector<VisualRecord>& visuals = cache.types->visuals();
    put_pod(out, static_cast<u32>(visuals.size()));
    {
        const usize at = out.size();
        out.resize(at + visuals.size() * sizeof(VisualRecord));
        std::memcpy(out.data() + at, visuals.data(), visuals.size() * sizeof(VisualRecord));
    }

    const std::vector<BehaviourRecord>& behaviours = cache.types->behaviours();
    put_pod(out, static_cast<u32>(behaviours.size()));
    for (const BehaviourRecord& record : behaviours) {
        put_pod(out, record.material);
        put_pod(out, record.script);
        const u64* words = record.tags.fast_words();
        for (u32 w = 0; w < kFastTagWords; ++w) put_pod(out, words[w]);
        put_pod(out, static_cast<u32>(record.tags.overflow().size()));
        for (TagId tag : record.tags.overflow()) put_pod(out, tag);
        put_pod(out, static_cast<u32>(record.properties.size()));
        for (const PropertyMap::Entry& entry : record.properties.entries()) {
            put_pod(out, entry.id);
            put_pod(out, entry.value.bits);
        }
    }

    const std::vector<VoxelType>& types = cache.types->types();
    put_pod(out, static_cast<u32>(types.size()));
    {
        const usize at = out.size();
        out.resize(at + types.size() * sizeof(VoxelType));
        std::memcpy(out.data() + at, types.data(), types.size() * sizeof(VoxelType));
    }

    put_pod(out, static_cast<u32>(cache.materials.size()));
    for (VoxelTypeId id : cache.materials) put_pod(out, id);

    // Which materials the despeckler may touch. Taken once over the whole clip and unobtainable
    // from anything a resuming run has to hand -- see CachedStipple. The flag goes out separately
    // from the list because "asked, and nothing had specks" and "never asked" are different
    // answers and the reader has to be able to tell them apart.
    put_pod(out, static_cast<u8>(cache.stipple_taken ? 1u : 0u));
    put_pod(out, static_cast<u32>(cache.stipple.size()));
    {
        std::vector<const CachedStipple*> order;
        order.reserve(cache.stipple.size());
        for (const CachedStipple& entry : cache.stipple) order.push_back(&entry);
        std::sort(order.begin(), order.end(),
                  [](const CachedStipple* a, const CachedStipple* b) { return a->type < b->type; });
        for (const CachedStipple* entry : order) {
            put_pod(out, entry->type);
            put_pod(out, static_cast<u8>(entry->may_despeckle ? 1u : 0u));
        }
    }

    // Where the lamps are, so a loaded world does not have to be read again to find them. R9g,
    // and the same argument as the ledger below: rediscovering them is the order of work this file
    // exists to skip.
    put_pod(out, static_cast<u32>(cache.emitters.size()));
    {
        std::vector<const CachedEmitters*> order;
        order.reserve(cache.emitters.size());
        for (const CachedEmitters& chunk : cache.emitters) order.push_back(&chunk);
        std::sort(order.begin(), order.end(),
                  [](const CachedEmitters* a, const CachedEmitters* b) {
                      return chunk_coord_less(ChunkCoord{a->chunk_x, a->chunk_y, a->chunk_z},
                                              ChunkCoord{b->chunk_x, b->chunk_y, b->chunk_z});
                  });
        for (const CachedEmitters* chunk : order) {
            put_pod(out, chunk->chunk_x);
            put_pod(out, chunk->chunk_y);
            put_pod(out, chunk->chunk_z);
            put_pod(out, static_cast<u32>(chunk->cells.size()));
            for (const EmissiveCell& cell : chunk->cells) put_pod(out, cell);
        }
    }

    // The ledger's running totals. Recomputing them means counting every voxel in the world,
    // which is the same order of work the cache exists to skip.
    if (cache.ledger != nullptr) {
        put_pod(out, static_cast<u32>(cache.ledger->totals().size()));
        std::vector<std::pair<VoxelTypeId, i64>> totals(cache.ledger->totals().begin(),
                                                        cache.ledger->totals().end());
        std::sort(totals.begin(), totals.end(),
                  [](const std::pair<VoxelTypeId, i64>& a, const std::pair<VoxelTypeId, i64>& b) {
                      return a.first < b.first;
                  });
        for (const auto& entry : totals) {
            put_pod(out, entry.first);
            put_pod(out, entry.second);
        }
    } else {
        put_pod(out, 0u);
    }
}

// The metadata as read, before any of it is put anywhere. Kept apart from the WorldCache because
// the edit-only path has to decide what to do about the type table BETWEEN reading it and adopting
// it, and because a journal only ever applies the LAST metadata block it finds — applying every
// one of them would intern the tags once per segment and add the ledger's totals up again each
// time.
struct MetaIn {
    std::vector<VisualRecord> visuals;
    std::vector<BehaviourRecord> behaviours;
    std::vector<VoxelType> types;
    std::vector<VoxelTypeId> materials;
    bool stipple_taken = false;
    std::vector<CachedStipple> stipple;
    std::vector<CachedEmitters> emitters;
    std::vector<std::pair<VoxelTypeId, i64>> ledger;
};

bool read_meta_block(Cursor& in, const WorldCache& cache, MetaIn& out) {
    const u32 tag_count = in.pod<u32>();
    if (!in.ok) return false;
    for (u32 i = 0; i < tag_count && in.ok; ++i) {
        const u32 length = in.pod<u32>();
        const u8* text = in.take(length);
        if (!in.ok) return false;
        if (cache.tags->intern(std::string(reinterpret_cast<const char*>(text), length)) != i) {
            return false;
        }
    }
    const u32 property_count = in.pod<u32>();
    if (!in.ok) return false;
    for (u32 i = 0; i < property_count && in.ok; ++i) {
        const u32 length = in.pod<u32>();
        const u8* text = in.take(length);
        if (!in.ok) return false;
        const auto type = static_cast<PropertyType>(in.pod<u8>());
        const auto domain = static_cast<PropertyDomain>(in.pod<u8>());
        const PropertyValue value{in.pod<u64>()};
        if (cache.properties->define(std::string(reinterpret_cast<const char*>(text), length), type,
                                     domain, value) != i) {
            return false;
        }
    }
    if (!in.ok) return false;

    const u32 visual_count = in.pod<u32>();
    if (!in.ok || !in.room_for(visual_count, sizeof(VisualRecord))) return false;
    out.visuals.resize(visual_count);
    {
        const u8* p = in.take(visual_count * sizeof(VisualRecord));
        if (!in.ok) return false;
        std::memcpy(out.visuals.data(), p, visual_count * sizeof(VisualRecord));
    }

    const u32 behaviour_count = in.pod<u32>();
    if (!in.ok || !in.room_for(behaviour_count, 8 + kFastTagWords * 8 + 8)) return false;
    out.behaviours.assign(behaviour_count, BehaviourRecord{});
    for (BehaviourRecord& record : out.behaviours) {
        record.material = in.pod<u32>();
        record.script = in.pod<u32>();
        for (u32 w = 0; w < kFastTagWords; ++w) {
            const u64 word = in.pod<u64>();
            for (u32 bit = 0; bit < 64; ++bit) {
                if ((word >> bit) & 1u) record.tags.add(w * 64 + bit);
            }
        }
        const u32 overflow = in.pod<u32>();
        for (u32 i = 0; i < overflow && in.ok; ++i) record.tags.add(in.pod<u32>());
        const u32 entries = in.pod<u32>();
        for (u32 i = 0; i < entries && in.ok; ++i) {
            const PropertyId id = in.pod<u32>();
            record.properties.set(id, PropertyValue{in.pod<u64>()});
        }
        if (!in.ok) return false;
    }

    const u32 type_count = in.pod<u32>();
    if (!in.ok || !in.room_for(type_count, sizeof(VoxelType))) return false;
    out.types.resize(type_count);
    {
        const u8* p = in.take(type_count * sizeof(VoxelType));
        if (!in.ok) return false;
        std::memcpy(out.types.data(), p, type_count * sizeof(VoxelType));
    }

    const u32 material_count = in.pod<u32>();
    if (!in.ok || !in.room_for(material_count, 4)) return false;
    out.materials.clear();
    out.materials.reserve(material_count);
    for (u32 i = 0; i < material_count && in.ok; ++i) out.materials.push_back(in.pod<u32>());
    if (!in.ok) return false;

    out.stipple_taken = in.pod<u8>() != 0u;
    const u32 stipple_count = in.pod<u32>();
    if (!in.ok || !in.room_for(stipple_count, 5)) return false;
    out.stipple.clear();
    out.stipple.reserve(stipple_count);
    for (u32 i = 0; i < stipple_count && in.ok; ++i) {
        CachedStipple entry;
        entry.type = in.pod<u32>();
        entry.may_despeckle = in.pod<u8>() != 0u;
        out.stipple.push_back(entry);
    }
    if (!in.ok) return false;

    const u32 emitter_chunks = in.pod<u32>();
    if (!in.ok || !in.room_for(emitter_chunks, 28)) return false;
    out.emitters.clear();
    out.emitters.reserve(emitter_chunks);
    for (u32 i = 0; i < emitter_chunks && in.ok; ++i) {
        CachedEmitters chunk;
        chunk.chunk_x = in.pod<i64>();
        chunk.chunk_y = in.pod<i64>();
        chunk.chunk_z = in.pod<i64>();
        const u32 cell_count = in.pod<u32>();
        if (!in.ok || !in.room_for(cell_count, sizeof(EmissiveCell))) return false;
        chunk.cells.reserve(cell_count);
        for (u32 c = 0; c < cell_count && in.ok; ++c) chunk.cells.push_back(in.pod<EmissiveCell>());
        out.emitters.push_back(std::move(chunk));
    }
    if (!in.ok) return false;

    const u32 ledger_count = in.pod<u32>();
    if (!in.ok || !in.room_for(ledger_count, 12)) return false;
    out.ledger.clear();
    out.ledger.reserve(ledger_count);
    for (u32 i = 0; i < ledger_count && in.ok; ++i) {
        const VoxelTypeId type = in.pod<u32>();
        const i64 total = in.pod<i64>();
        out.ledger.emplace_back(type, total);
    }
    return in.ok;
}

// Every live brick of a chunk, as the file holds them. Returns how many there were.
u32 write_chunk_bricks(std::vector<u8>& out, const Chunk& chunk) {
    const u32 axis = static_cast<u32>(kChunkBricks);
    u32 bricks = 0;
    for (u32 bz = 0; bz < axis; ++bz) {
        for (u32 by = 0; by < axis; ++by) {
            for (u32 bx = 0; bx < axis; ++bx) {
                const Brick* brick = chunk.brick(bx, by, bz);
                if (!brick_is_live(brick)) continue;
                put_pod(out, brick_slot(bx, by, bz));
                write_brick_raw(out, *brick);
                ++bricks;
            }
        }
    }
    return bricks;
}

// The brick slots a person EMPTIED — R12d, and the half a flag on a brick cannot carry, because
// there is no brick left to carry it.
//
// A carve takes the last voxel out of a brick, the brick is unlinked (it must be: an empty brick
// left allocated is a lump the marcher draws and can never build, D348/D620), and the hole is then
// indistinguishable from sky nobody ever touched. Written as two bytes a slot, and only for the
// chunks that have any — a world nobody has carved writes one zero per chunk.
u32 write_chunk_erased(std::vector<u8>& out, const Chunk& chunk) {
    const usize count_at = out.size();
    put_pod(out, static_cast<u32>(0));
    if (!edit_tracking() || chunk.erased_bricks() == 0) return 0;

    const u32 axis = static_cast<u32>(kChunkBricks);
    u32 count = 0;
    for (u32 bz = 0; bz < axis; ++bz) {
        for (u32 by = 0; by < axis; ++by) {
            for (u32 bx = 0; bx < axis; ++bx) {
                if (!chunk.brick_erased(bx, by, bz)) continue;
                put_pod(out, brick_slot(bx, by, bz));
                ++count;
            }
        }
    }
    std::memcpy(out.data() + count_at, &count, sizeof(count));
    return count;
}

// What a whole-world write has to put in the file, and what it can leave where it is.
//
// The comparison is over the ENCODED BYTES of a chunk rather than over `Chunk::content_hash`, and
// the difference matters in both directions. Content hashing decodes all sixteen million voxels of
// a chunk to answer, which is the order of work the cache exists to avoid; encoding is a memcpy of
// what is already held and has to be done anyway for the chunks that DID change. And a byte hash
// cannot say "unchanged" about a chunk that is not — two identical byte strings decode to the same
// voxels by construction — where the reverse mistake, calling a re-encoded but identical chunk
// changed, costs a rewrite of that chunk and nothing else.
struct ChunkPlan {
    u32 written = 0;
    u32 left_alone = 0;
    u32 bricks = 0;
    std::vector<DirectoryChunk> directory;   // every live chunk, whether written now or before
    std::vector<ChunkCoord> dropped;
};

// Straight into the journal, and a chunk that turns out to be already there is rolled back off the
// end of it. There is no scratch buffer and no second copy: the first whole write of a finished
// estate is several hundred megabytes, and holding two of it to decide what to keep would cost
// more than the write.
ChunkPlan write_changed_chunks(std::vector<u8>& out, const World& world,
                               const CacheDirectory* was) {
    ChunkPlan plan;
    std::unordered_map<ChunkCoord, u64, ChunkCoordHash> before;
    if (was != nullptr) {
        before.reserve(was->chunks.size() * 2 + 1);
        for (const DirectoryChunk& chunk : was->chunks) {
            before.emplace(ChunkCoord{chunk.x, chunk.y, chunk.z}, chunk.hash);
        }
    }

    for (const ChunkCoord& coord : world.sorted_chunk_coords()) {
        const Chunk* chunk = world.chunk(coord);
        if (!chunk_is_live(chunk)) continue;

        const usize record_at = out.size();
        put_pod(out, coord.x);
        put_pod(out, coord.y);
        put_pod(out, coord.z);
        const usize count_at = out.size();
        put_pod(out, static_cast<u32>(0));
        const usize bricks_at = out.size();
        const u32 bricks = write_chunk_bricks(out, *chunk);
        std::memcpy(out.data() + count_at, &bricks, sizeof(bricks));
        // Inside the hashed range on purpose: a chunk whose only change since the last save is a
        // brick somebody carved away has no bricks that moved, and if the erased list sat outside
        // the hash the increment would call it unchanged and the carve would not be banked.
        write_chunk_erased(out, *chunk);
        const u64 hash = hash_span(out.data() + bricks_at, out.size() - bricks_at);
        plan.directory.push_back(DirectoryChunk{coord.x, coord.y, coord.z, hash});

        const auto found = before.find(coord);
        const bool already_there = found != before.end() && found->second == hash;
        if (found != before.end()) before.erase(found);
        if (already_there) {
            out.resize(record_at);
            ++plan.left_alone;
            continue;
        }
        ++plan.written;
        plan.bricks += bricks;
    }

    // Whatever the file had and the world no longer does. Said out loud in the increment, because
    // a journal that only ever adds would bring a demolished outbuilding back on the next load.
    plan.dropped.reserve(before.size());
    for (const auto& [coord, hash] : before) plan.dropped.push_back(coord);
    std::sort(plan.dropped.begin(), plan.dropped.end(), chunk_coord_less);
    return plan;
}

// One segment, opened where the journal now ends and closed once its payload is in.
usize open_segment(std::vector<u8>& journal) {
    const usize at = journal.size();
    put_pod(journal, kSegmentMagic);
    put_pod(journal, static_cast<u32>(0));
    put_pod(journal, static_cast<u64>(0));
    return at;
}

void close_segment(std::vector<u8>& journal, usize at, u32 kind) {
    const u64 bytes = static_cast<u64>(journal.size() - at);
    std::memcpy(journal.data() + at + 4, &kind, sizeof(kind));
    std::memcpy(journal.data() + at + 8, &bytes, sizeof(bytes));
}

struct SegmentSpan {
    u32 kind = 0;
    const u8* data = nullptr;
    usize size = 0;
};

// Walking the journal is the only structural check the reader makes before it trusts anything:
// every segment must carry the magic, must fit inside what the header claims, and the last one
// must end exactly where the journal does. A file whose append was cut short fails all three.
bool walk_journal(const u8* blob, usize blob_size, const CacheHeader& head,
                  std::vector<SegmentSpan>& out) {
    // SHORTER than the header committed to is a truncated file and is refused. LONGER is a tail
    // from an append that did not finish, and it is not a fault: `journal_bytes` is what the file
    // is committed to, the check hash covers it, and bytes past it were never claimed. Stopping at
    // the committed end is not leniency -- it is reading exactly what was committed.
    if (kHeaderBytes + head.journal_bytes > blob_size) return false;
    usize at = kHeaderBytes;
    const usize end = kHeaderBytes + static_cast<usize>(head.journal_bytes);
    for (u32 i = 0; i < head.segments; ++i) {
        if (at + kSegmentHeaderBytes > end) return false;
        u32 magic = 0, kind = 0;
        u64 bytes = 0;
        std::memcpy(&magic, blob + at + 0, sizeof(magic));
        std::memcpy(&kind, blob + at + 4, sizeof(kind));
        std::memcpy(&bytes, blob + at + 8, sizeof(bytes));
        if (magic != kSegmentMagic) return false;
        if (bytes < kSegmentHeaderBytes || at + static_cast<usize>(bytes) > end) return false;
        out.push_back(SegmentSpan{kind, blob + at + kSegmentHeaderBytes,
                                  static_cast<usize>(bytes) - kSegmentHeaderBytes});
        at += static_cast<usize>(bytes);
    }
    return at == end;
}

// The header a file has to have for the next save to be an append rather than a rewrite.
bool open_for_append(const std::string& path, u64 key, CacheHeader& head, CacheDirectory& dir) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    u8 raw[kHeaderBytes];
    if (!file.read(reinterpret_cast<char*>(raw), kHeaderBytes)) return false;
    if (!decode_header(raw, kHeaderBytes, head)) return false;
    if (head.key != key) return false;
    if (head.mode != static_cast<u8>(WorldCacheMode::Whole)) return false;
    if (head.segments >= kMaxSegments) return false;
    // At LEAST as long as the header committed to. Longer is what a crash part way through an
    // earlier append leaves behind, and it is not a fault: those bytes were never committed, this
    // append writes over them, and the trim at the end takes back whatever is left.
    if (static_cast<u64>(size) < kHeaderBytes + head.journal_bytes) return false;
    // Past a doubling the journal is rewritten whole rather than grown further. See kMaxSegments.
    if (head.base_bytes == 0 || head.journal_bytes >= 2 * head.base_bytes) return false;

    if (head.directory_at < kHeaderBytes ||
        head.directory_at + kSegmentHeaderBytes > kHeaderBytes + head.journal_bytes) {
        return false;
    }
    file.seekg(static_cast<std::streamoff>(head.directory_at));
    u8 segment[kSegmentHeaderBytes];
    if (!file.read(reinterpret_cast<char*>(segment), kSegmentHeaderBytes)) return false;
    u32 magic = 0, kind = 0;
    u64 bytes = 0;
    std::memcpy(&magic, segment + 0, sizeof(magic));
    std::memcpy(&kind, segment + 4, sizeof(kind));
    std::memcpy(&bytes, segment + 8, sizeof(bytes));
    if (magic != kSegmentMagic || kind != kSegmentDirectory) return false;
    // The directory is always the last thing in the journal, which is what makes the next append
    // able to find it in one seek.
    if (bytes < kSegmentHeaderBytes ||
        head.directory_at + bytes != kHeaderBytes + head.journal_bytes) {
        return false;
    }
    std::vector<u8> body(static_cast<usize>(bytes) - kSegmentHeaderBytes);
    if (!body.empty() &&
        !file.read(reinterpret_cast<char*>(body.data()),
                   static_cast<std::streamsize>(body.size()))) {
        return false;
    }
    Cursor in{body.data(), body.size(), 0, true};
    return read_directory(in, dir);
}

}  // namespace

u64 world_cache_key(const std::string& source_text, i32 voxels_per_metre, u64 build_stamp) {
    u64 h = hash_mix(kVersion);
    h = hash_combine(h, build_stamp);
    h = hash_combine(h, static_cast<u64>(voxels_per_metre));
    h = hash_combine(h, static_cast<u64>(source_text.size()));
    for (char c : source_text) h = hash_combine(h, static_cast<u64>(static_cast<u8>(c)));
    return h;
}

std::vector<CachedEditBox> edit_boxes_from_ops(const std::vector<Op>& ops) {
    std::vector<CachedEditBox> out;
    out.reserve(std::min(ops.size(), kMaxEditBoxes));
    usize refused = 0;
    for (const Op& op : ops) {
        Op box = op;
        box.normalise();   // corners in either order; a box is a box whichever way it was cut
        CachedEditBox named;
        named.low[0] = box.x0;
        named.low[1] = box.y0;
        named.low[2] = box.z0;
        named.high[0] = box.x1;
        named.high[1] = box.y1;
        named.high[2] = box.z1;

        // Exact duplicates and boxes already covered. A chisel held down produces the same box
        // many times over, and an undo of a stroke is the stroke's own box again -- neither adds
        // anything to "which bricks were touched", which is the only question this list answers.
        //
        // Backwards, and only over the tail: a full pairwise containment test is quadratic in the
        // length of an evening, and the boxes that repeat are the ones that repeat immediately.
        // What this misses is a box covered by something written an hour earlier, which costs two
        // bytes and never costs a building.
        bool covered = false;
        const usize look_back = out.size() < 64 ? out.size() : 64;
        for (usize i = 0; i < look_back; ++i) {
            if (box_contains(out[out.size() - 1 - i], named)) {
                covered = true;
                break;
            }
        }
        if (covered) continue;

        if (out.size() >= kMaxEditBoxes) {
            ++refused;
            continue;
        }
        out.push_back(named);
    }
    if (refused > 0) {
        // Said out loud, because what it means is that the file no longer knows about some of
        // what somebody did. Everything those ops CHANGED is still in the difference; what is
        // lost is only the part a difference cannot see.
        WS_LOG_WARN("cache",
                    "{} of {} edits are past the {} the file will name; their changes are still "
                    "written, but a brick they touched and left agreeing with the clip is not",
                    refused, ops.size(), kMaxEditBoxes);
    }
    return out;
}

bool write_world_cache(const std::string& path, u64 key, const WorldCache& cache,
                       WorldCacheWritten* written) {
    if (written != nullptr) *written = WorldCacheWritten{};
    if (cache.world == nullptr || cache.types == nullptr) return false;

    // A world differenced against itself is empty by construction, so this would write a file
    // saying "the world is exactly what the clip builds" over a world somebody has spent an
    // evening carving, and it would say it without a single warning. Refused rather than
    // documented: there is no caller for whom this is what they meant.
    if (cache.baseline == cache.world) {
        WS_LOG_WARN("cache", "refusing to write '{}' as a difference from itself", path);
        return false;
    }
    const bool edit_only = cache.baseline != nullptr;

    // Timed, and reported, because this now runs while somebody is playing rather than only at
    // the end of a build. A cost nobody can see is a cost nobody can weigh against the stall it
    // is meant to be saving.
    const u64 began = now_ns();

    std::vector<u8> meta;
    meta.reserve(1u << 16);
    write_meta_block(meta, cache);
    const u64 meta_hash = hash_span(meta.data(), meta.size());
    const std::vector<u64> blocks = region_block_hashes(cache.regions);

    // Can this be an append? Only a whole-world file that this build wrote, that was finished, and
    // whose journal has not yet doubled. Everything else is a rewrite, which is also what the very
    // first save is.
    CacheHeader was{};
    CacheDirectory before;
    bool append = false;
    if (!edit_only) append = open_for_append(path, key, was, before);

    u32 written_bricks = 0;
    u32 cleared_bricks = 0;
    u32 kept_bricks = 0;
    u32 fingerprinted = 0;
    u32 region_blocks_written = static_cast<u32>(blocks.size());
    ChunkPlan plan;
    // The journal's new tail, built once and written once. Everything below goes straight into it
    // rather than into a payload that is then copied into a segment that is then copied into a
    // file: the first whole write of a finished estate is several hundred megabytes and each of
    // those copies would be another one of it in memory.
    std::vector<u8> payload;
    payload.reserve(1u << 22);
    const usize segment = open_segment(payload);
    u32 kind = kSegmentFull;

    if (!edit_only) {
        const bool meta_moved = !append || before.meta_hash != meta_hash;
        std::vector<u32> moved_blocks;
        for (u32 block = 0; block < static_cast<u32>(blocks.size()); ++block) {
            if (!append || block >= before.region_blocks.size() ||
                before.region_blocks[block] != blocks[block]) {
                moved_blocks.push_back(block);
            }
        }
        const bool regions_moved =
            !append || !moved_blocks.empty() ||
            before.regions != static_cast<u32>(cache.regions.size());
        kind = append ? kSegmentIncrement : kSegmentFull;
        region_blocks_written = regions_moved ? static_cast<u32>(moved_blocks.size()) : 0u;

        {
            const usize part = open_part(payload);
            if (meta_moved) payload.insert(payload.end(), meta.begin(), meta.end());
            close_part(payload, part);
        }
        {
            const usize part = open_part(payload);
            if (!append) {
                // A whole world states the leaf set outright; the blocks are then implicit, a
                // thousand leaves at a time, and every increment after this one names the ones it
                // moves.
                put_pod(payload, static_cast<u32>(cache.regions.size()));
                for (const CachedRegion& region : cache.regions) write_region(payload, region);
            } else if (regions_moved) {
                put_pod(payload, static_cast<u32>(cache.regions.size()));
                put_pod(payload, static_cast<u32>(moved_blocks.size()));
                for (u32 block : moved_blocks) {
                    const u32 from = block * kRegionsPerBlock;
                    const u32 to = std::min<u32>(from + kRegionsPerBlock,
                                                 static_cast<u32>(cache.regions.size()));
                    put_pod(payload, block);
                    put_pod(payload, to - from);
                    const usize span = open_part(payload);
                    for (u32 i = from; i < to; ++i) write_region(payload, cache.regions[i]);
                    close_part(payload, span);
                }
            }
            close_part(payload, part);
        }
        {
            const usize part = open_part(payload);
            const usize count_at = payload.size();
            put_pod(payload, static_cast<u32>(0));
            plan = write_changed_chunks(payload, *cache.world, append ? &before : nullptr);
            std::memcpy(payload.data() + count_at, &plan.written, sizeof(plan.written));
            close_part(payload, part);
            written_bricks = plan.bricks;
        }
        if (append) {
            const usize part = open_part(payload);
            put_pod(payload, static_cast<u32>(plan.dropped.size()));
            for (const ChunkCoord& coord : plan.dropped) {
                put_pod(payload, coord.x);
                put_pod(payload, coord.y);
                put_pod(payload, coord.z);
            }
            close_part(payload, part);
        }

        // NOTHING HAS CHANGED SINCE THE LAST SAVE, so nothing is written.
        //
        // This is not a micro-optimisation, it is D721's own waste: a resumed run reaches the
        // fixed point again with `refine_saved_regions_` back at nought, decides the world is
        // worth keeping, and rewrites hundreds of megabytes to say exactly what the file already
        // said. The file is the authority on whether that is true, and it answers in a hash per
        // chunk.
        if (append && !meta_moved && !regions_moved && plan.written == 0 && plan.dropped.empty()) {
            WS_LOG_INFO("cache",
                        "'{}' already holds this world -- {} chunks, {} leaves, nothing written "
                        "({:.0f} ms to find out)",
                        path, plan.left_alone, cache.regions.size(), ns_to_ms(now_ns() - began));
            if (written != nullptr) {
                written->incremental = true;
                written->unchanged = true;
                written->chunks_left_alone = plan.left_alone;
                written->region_blocks_total = static_cast<u32>(blocks.size());
                written->file_bytes = kHeaderBytes + was.journal_bytes;
            }
            return true;
        }
    } else {
        // ---- R11f: the difference from what the clip builds ---------------------------------
        {
            const usize part = open_part(payload);
            payload.insert(payload.end(), meta.begin(), meta.end());
            close_part(payload, part);
        }
        {
            const usize part = open_part(payload);
            put_pod(payload, static_cast<u32>(cache.regions.size()));
            for (const CachedRegion& region : cache.regions) write_region(payload, region);
            close_part(payload, part);
        }

        // What was edited, if the writer was told. Separate flag, empty list: see
        // WorldCache::edits_named. Written before the difference because it is what decides how
        // much of the difference there is.
        {
            const usize part = open_part(payload);
            put_pod(payload, static_cast<u8>(cache.edits_named ? 1u : 0u));
            put_pod(payload, static_cast<u32>(cache.edited.size()));
            for (const CachedEditBox& box : cache.edited) {
                for (i64 v : box.low) put_pod(payload, v);
                for (i64 v : box.high) put_pod(payload, v);
            }
            close_part(payload, part);
        }

        // The fingerprint of the baseline this difference was taken against, chunk by chunk.
        //
        // Without it the reader has no way to know that the world it is laying edits over is the
        // world they were cut out of, and the failure is silent by construction: a difference
        // applies cleanly to any world at all, and produces one that is right where somebody
        // carved and quietly wrong everywhere else. Per chunk rather than one number for the
        // world, because a mismatch that can be NAMED is a mismatch somebody can diagnose, and
        // because the reader can then say how much of the world it disagrees about.
        const std::vector<ChunkCoord> base_live = live_chunk_coords(*cache.baseline);
        fingerprinted = static_cast<u32>(base_live.size());
        {
            const usize part = open_part(payload);
            put_pod(payload, static_cast<u32>(base_live.size()));
            for (const ChunkCoord& coord : base_live) {
                put_pod(payload, coord.x);
                put_pod(payload, coord.y);
                put_pod(payload, coord.z);
                put_pod(payload, cache.baseline->chunk_hash(coord));
            }
            close_part(payload, part);
        }

        // Every chunk either world has anything in, in one order.
        const std::vector<ChunkCoord> world_live = live_chunk_coords(*cache.world);
        std::vector<ChunkCoord> all = world_live;
        all.insert(all.end(), base_live.begin(), base_live.end());

        // AND EVERY CHUNK A NAMED BOX REACHES, WHETHER OR NOT ANYTHING IS IN IT — data-loss
        // case 1, and it is the named-box guarantee failing in exactly the place it was written
        // for.
        //
        // A named box says "somebody's hands were here, write these bricks down whatever they
        // hold", and the whole reason it exists is the swing through open air: no difference to
        // see, nothing but the box to say it happened. But the walk below was over the chunks the
        // two worlds have BETWEEN them, so a swing in a chunk neither of them has anything in --
        // a chisel in an empty field, the last carve out of a demolished outbuilding -- reached no
        // chunk in the list and was written nowhere. The file came back saying nothing about it,
        // "the file does not mention it" means "leave the clip's answer alone", and the day the
        // clip grows something there the swing is filled in.
        //
        // Bounded, because a box is somebody else's number: a `fill` from a console covers as many
        // chunks as it likes and this list is walked once per chunk. Past the bound the empty
        // chunks are dropped and said out loud -- nothing that EXISTS is ever dropped, because
        // every live chunk of either world is already in the list above.
        constexpr usize kMaxNamedEmptyChunks = 1u << 16;
        usize named_chunks = 0;
        bool named_chunks_clipped = false;
        for (const CachedEditBox& box : cache.edited) {
            const i64 cx0 = chunk_index_of(box.low[0]), cx1 = chunk_index_of(box.high[0]);
            const i64 cy0 = chunk_index_of(box.low[1]), cy1 = chunk_index_of(box.high[1]);
            const i64 cz0 = chunk_index_of(box.low[2]), cz1 = chunk_index_of(box.high[2]);
            for (i64 cz = cz0; cz <= cz1 && !named_chunks_clipped; ++cz) {
                for (i64 cy = cy0; cy <= cy1 && !named_chunks_clipped; ++cy) {
                    for (i64 cx = cx0; cx <= cx1; ++cx) {
                        if (++named_chunks > kMaxNamedEmptyChunks) {
                            named_chunks_clipped = true;
                            break;
                        }
                        all.push_back(ChunkCoord{cx, cy, cz});
                    }
                }
            }
            if (named_chunks_clipped) break;
        }
        if (named_chunks_clipped) {
            WS_LOG_WARN("cache",
                        "the named edit boxes span more than {} chunks; the ones neither the world "
                        "nor its clip has anything in are not written",
                        kMaxNamedEmptyChunks);
        }

        std::sort(all.begin(), all.end(), chunk_coord_less);
        all.erase(std::unique(all.begin(), all.end()), all.end());

        // Chunks the baseline fills and the world does not. An emptied chunk has to be said out
        // loud: "the file does not mention it" already means "leave the clip's answer alone", so
        // a room somebody demolished would come back built.
        std::vector<ChunkCoord> gone;
        for (const ChunkCoord& coord : all) {
            if (!chunk_is_live(cache.world->chunk(coord)) &&
                chunk_is_live(cache.baseline->chunk(coord))) {
                gone.push_back(coord);
            }
        }
        {
            const usize part = open_part(payload);
            put_pod(payload, static_cast<u32>(gone.size()));
            for (const ChunkCoord& coord : gone) {
                put_pod(payload, coord.x);
                put_pod(payload, coord.y);
                put_pod(payload, coord.z);
            }
            close_part(payload, part);
        }

        const usize changed_part = open_part(payload);
        u32 changed_chunks = 0;
        const usize changed_at = payload.size();
        put_pod(payload, 0u);

        const u32 axis = static_cast<u32>(kChunkBricks);
        std::vector<const CachedEditBox*> reaching;
        std::vector<u16> clears;
        std::vector<u8> writes;
        for (const ChunkCoord& coord : all) {
            const Chunk* chunk = cache.world->chunk(coord);
            const Chunk* base = cache.baseline->chunk(coord);
            // An emptied chunk is already in `gone` and is written there whole. A chunk live in
            // NEITHER world is not skipped, and that is data-loss case 1: it is where a named box
            // in empty space lands, and the loop below writes its clearings.
            if (!chunk_is_live(chunk) && chunk_is_live(base)) continue;

            // Which named edit boxes reach into this chunk at all, so the per-brick test below is
            // against a handful of boxes rather than the whole log.
            const i64 chunk_low[3] = {coord.x * kChunkEdge, coord.y * kChunkEdge,
                                      coord.z * kChunkEdge};
            const i64 chunk_high[3] = {chunk_low[0] + kChunkEdge - 1, chunk_low[1] + kChunkEdge - 1,
                                       chunk_low[2] + kChunkEdge - 1};
            reaching.clear();
            for (const CachedEditBox& box : cache.edited) {
                if (box_touches(box, chunk_low, chunk_high)) reaching.push_back(&box);
            }

            clears.clear();
            writes.clear();
            u32 write_count = 0;
            for (u32 bz = 0; bz < axis; ++bz) {
                for (u32 by = 0; by < axis; ++by) {
                    for (u32 bx = 0; bx < axis; ++bx) {
                        const Brick* mine = (chunk != nullptr) ? chunk->brick(bx, by, bz) : nullptr;
                        const Brick* theirs = (base != nullptr) ? base->brick(bx, by, bz) : nullptr;
                        const bool mine_live = brick_is_live(mine);
                        const bool theirs_live = brick_is_live(theirs);
                        // R12d, and it is the same guarantee `named` gives one level coarser: the
                        // chunk itself knows this slot was emptied by a person, whether or not any
                        // box was named and whether or not the clip agrees about it today. The
                        // early-out below would drop it, so it is asked for here.
                        const bool erased = edit_tracking() && chunk != nullptr &&
                                            chunk->erased_bricks() != 0 &&
                                            chunk->brick_erased(bx, by, bz);
                        if (!mine_live && !theirs_live && reaching.empty() && !erased) continue;

                        bool named = false;
                        if (!reaching.empty()) {
                            const i64 low[3] = {chunk_low[0] + static_cast<i64>(bx) * kBrickEdge,
                                                chunk_low[1] + static_cast<i64>(by) * kBrickEdge,
                                                chunk_low[2] + static_cast<i64>(bz) * kBrickEdge};
                            const i64 high[3] = {low[0] + kBrickEdge - 1, low[1] + kBrickEdge - 1,
                                                 low[2] + kBrickEdge - 1};
                            for (const CachedEditBox* box : reaching) {
                                if (box_touches(*box, low, high)) {
                                    named = true;
                                    break;
                                }
                            }
                        }

                        if (mine_live) {
                            // R12d closes the same hole `named` does, from inside the world rather
                            // than from the op log. A brick somebody carved and filled back in with
                            // the clip's own stone agrees with the clip TODAY and would be left out
                            // — and comes back as whatever the clip says the day the clip moves,
                            // with no record a person ever chose it. The brick knows; ask it, so
                            // guarantee no longer depends on a writer having been handed the boxes.
                            const bool owned = edit_tracking() && mine->edited();
                            if (!named && !owned && theirs_live &&
                                bricks_identical(*mine, *theirs)) {
                                ++kept_bricks;   // the clip's own answer; not written at all
                                continue;
                            }
                            const usize at = writes.size();
                            writes.resize(at + sizeof(u16));
                            const u16 slot = brick_slot(bx, by, bz);
                            std::memcpy(writes.data() + at, &slot, sizeof(slot));
                            write_brick_raw(writes, *mine);
                            ++write_count;
                            ++written_bricks;
                        } else if (theirs_live || named || erased) {
                            // Air, said rather than left out. `theirs_live` is the carve that took
                            // away matter the clip puts back, and `named` is the same carve
                            // through air the clip happens to agree about today -- which costs two
                            // bytes and is the difference between a hole that survives a change to
                            // the clip and one that fills itself in.
                            clears.push_back(brick_slot(bx, by, bz));
                            ++cleared_bricks;
                        }
                    }
                }
            }
            if (clears.empty() && writes.empty()) continue;

            put_pod(payload, coord.x);
            put_pod(payload, coord.y);
            put_pod(payload, coord.z);
            put_pod(payload, static_cast<u32>(clears.size()));
            for (u16 slot : clears) put_pod(payload, slot);
            put_pod(payload, write_count);
            payload.insert(payload.end(), writes.begin(), writes.end());
            ++changed_chunks;
        }
        std::memcpy(payload.data() + changed_at, &changed_chunks, sizeof(changed_chunks));
        close_part(payload, changed_part);

        WS_LOG_INFO("cache",
                    "'{}' as the clip plus its edits: {} bricks written, {} cleared, {} left to "
                    "the clip; {} chunks fingerprinted, {} edit boxes named{}",
                    path, written_bricks, cleared_bricks, kept_bricks, base_live.size(),
                    cache.edited.size(),
                    cache.edits_named ? "" : " (nobody said what was edited)");
    }

    // The segment, and the directory that follows it, as one block of bytes to put on disk.
    close_segment(payload, segment, kind);
    const u64 segment_bytes = static_cast<u64>(payload.size());

    CacheDirectory now;
    now.chunks = plan.directory;
    now.regions = static_cast<u32>(cache.regions.size());
    now.region_blocks = blocks;
    now.meta_hash = meta_hash;
    const u64 directory_at =
        append ? kHeaderBytes + was.journal_bytes + segment_bytes : kHeaderBytes + segment_bytes;
    const usize directory_segment = open_segment(payload);
    write_directory(payload, now);
    close_segment(payload, directory_segment, kSegmentDirectory);
    std::vector<u8>& tail = payload;

    CacheHeader head;
    head.key = key;
    head.mode = static_cast<u8>(edit_only ? WorldCacheMode::EditOnly : WorldCacheMode::Whole);
    head.segments = append ? was.segments + 2 : 2;
    head.journal_bytes = (append ? was.journal_bytes : 0) + static_cast<u64>(tail.size());
    head.directory_at = directory_at;
    head.base_bytes = append ? was.base_bytes : segment_bytes;
    head.generation = was.generation + 1;

    u8 raw[kHeaderBytes];
    const u64 total = kHeaderBytes + head.journal_bytes;

    if (append) {
        // ---- the append, and every moment it can be interrupted at --------------------------
        //
        // NOTHING ALREADY COMMITTED IS TOUCHED UNTIL THE LAST SIXTY-FOUR BYTES. The new segments
        // go at `kHeaderBytes + was.journal_bytes`, which is one past the end of what the header
        // claims, so every byte written before the last line is a byte no reader will look at.
        // Then one 64-byte header write moves `journal_bytes` over them and commits the lot.
        //
        // That is the whole reason it is arranged this way, and there are three moments a machine
        // can stop:
        //
        //   - before any of the tail lands: the file is byte for byte what it was, and it reads
        //     back as the world the last save left;
        //   - part way through the tail: the extra bytes sit past the committed end, and it reads
        //     back as the world the last save left;
        //   - after the whole tail and before the header: the same again, because every one of
        //     those bytes is uncommitted until the header says otherwise.
        //
        // Only the 64-byte header write itself is unprotected, and it is one write inside a single
        // sector of a file that is already long enough. A torn one is caught by the check hash and
        // the file is refused -- that is the one case that costs the cache, and it is the smallest
        // window this format can be reduced to without carrying two headers.
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!file) {
            WS_LOG_WARN("cache", "could not open '{}' to append to", path);
            return false;
        }
        file.seekp(static_cast<std::streamoff>(kHeaderBytes + was.journal_bytes));
        file.write(reinterpret_cast<const char*>(tail.data()),
                   static_cast<std::streamsize>(tail.size()));
        file.flush();
        if (!file) {
            // The old header is still on disk and still committed to the old journal, so what is
            // on disk is the world the last save left. There is nothing to undo.
            WS_LOG_WARN("cache", "could not append to '{}'; the world it already held stands",
                        path);
            return false;
        }
        encode_header(raw, head);
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(raw), kHeaderBytes);
        file.flush();
        const bool ok = static_cast<bool>(file);
        file.close();
        if (!ok) {
            WS_LOG_WARN("cache", "could not finish '{}'; the world it already held stands", path);
            return false;
        }
        // AFTER the commit and never before it. What this takes back is the tail left by an append
        // that did not finish, when it was longer than the one just written; trimming first would
        // cut into bytes this write is about to commit to.
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (!error && size > total) {
            std::error_code trim;
            std::filesystem::resize_file(path, total, trim);
            // Not a failure, and it must not be reported as one. A tail past the committed end is
            // exactly what the reader ignores, so a file that could not be trimmed is a correct
            // file that is bigger than it needs to be, and the next save writes over it.
            if (trim) {
                WS_LOG_WARN("cache", "could not trim '{}' to {} bytes; it is still the world",
                            path, total);
            }
        }
    } else {
        // Written under a temporary name and renamed, so an interrupted run leaves the previous
        // cache intact rather than a half-file that passes its own header check.
        encode_header(raw, head);
        const std::string temporary = path + ".part";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) {
                WS_LOG_WARN("cache", "could not open '{}' for writing", temporary);
                return false;
            }
            file.write(reinterpret_cast<const char*>(raw), kHeaderBytes);
            file.write(reinterpret_cast<const char*>(tail.data()),
                       static_cast<std::streamsize>(tail.size()));
            if (!file) {
                WS_LOG_WARN("cache", "could not write '{}'", temporary);
                return false;
            }
        }
        std::remove(path.c_str());
        if (std::rename(temporary.c_str(), path.c_str()) != 0) {
            WS_LOG_WARN("cache", "could not rename '{}' into place", temporary);
            return false;
        }
    }

    const u64 put_down = append ? static_cast<u64>(tail.size()) : total;
    if (written != nullptr) {
        written->bricks_written = written_bricks;
        written->bricks_cleared = cleared_bricks;
        written->bricks_left_to_the_clip = kept_bricks;
        written->chunks_fingerprinted = fingerprinted;
        written->incremental = append;
        written->chunks_written = plan.written;
        written->chunks_left_alone = plan.left_alone;
        written->chunks_dropped = static_cast<u32>(plan.dropped.size());
        written->region_blocks_written = region_blocks_written;
        written->region_blocks_total = static_cast<u32>(blocks.size());
        written->bytes_written = put_down;
        written->file_bytes = total;
    }
    WS_LOG_INFO("cache", "wrote '{}' ({} MB in {:.0f} ms)", path, put_down / (1024 * 1024),
                ns_to_ms(now_ns() - began));
    if (!edit_only) {
        WS_LOG_INFO("cache",
                    "  {}: {} bytes for {} chunks written and {} left where they were, {} dropped; "
                    "{} leaves; the file is {} bytes in {} segments",
                    append ? "appended" : "whole", put_down, plan.written, plan.left_alone,
                    plan.dropped.size(), cache.regions.size(), total, head.segments);
    }
    return true;
}

// Does the file on disk belong to this key, without reading the file.
//
// Sixty-four bytes rather than six hundred megabytes. The caller uses it to decide whether a cached
// world is dead and should be deleted, and reading a third of a gigabyte off a disk to answer that
// would cost more than the rebuild it is trying to avoid announcing.
//
// A file left `running` by an interrupted append answers NO, which is the whole point of the field:
// the caller deletes it and the world is rebuilt, rather than a partial journal being loaded as a
// world.
bool world_cache_matches(const std::string& path, u64 key) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    u8 raw[kHeaderBytes];
    if (!file.read(reinterpret_cast<char*>(raw), kHeaderBytes)) return false;
    CacheHeader head;
    if (!decode_header(raw, kHeaderBytes, head)) return false;
    if (head.key != key) return false;
    // And it must be at least as long as its header committed to, because the caller uses this to
    // decide whether to delete the file: a truncated journal answers NO here and is rebuilt,
    // rather than being opened and refused later.
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || static_cast<u64>(size) < kHeaderBytes + head.journal_bytes) return false;
    return true;
}

// The mode byte, and a caller has to have it before it commits to anything.
//
// An edit-only file cannot be opened without the world its clip builds, and building that world is
// the expensive thing the caller is deciding about. Discovering the requirement inside the read —
// after the type table, after the region list — leaves the caller holding a failure it could have
// been told about for sixty-four bytes.
bool world_cache_mode_of(const std::string& path, WorldCacheMode& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    u8 raw[kHeaderBytes];
    if (!file.read(reinterpret_cast<char*>(raw), kHeaderBytes)) return false;
    CacheHeader head;
    if (!decode_header(raw, kHeaderBytes, head)) return false;
    out = static_cast<WorldCacheMode>(head.mode);
    return true;
}

bool read_world_cache(const std::string& path, u64 key, WorldCache& cache, JobSystem* jobs) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size <= 0 || static_cast<usize>(size) < kHeaderBytes) return false;
    file.seekg(0);

    std::vector<u8> blob(static_cast<usize>(size));
    if (!file.read(reinterpret_cast<char*>(blob.data()), size)) return false;

    CacheHeader head;
    if (!decode_header(blob.data(), blob.size(), head)) return false;
    if (head.key != key) return false;   // built from different source; not an error

    // THE HEADER IS THE COMMIT RECORD, so what it says the journal is, is what this reads.
    //
    // A file longer than `kHeaderBytes + journal_bytes` is what a machine that stopped part way
    // through an append leaves behind, and those bytes were never committed to: the world this
    // reads is the one the last finished save left, whole and by its own content hash. Said out
    // loud rather than passed over, because a cache that is quietly bigger than it claims is worth
    // one line to a person looking at why a file grew.
    //
    // Shorter, or a journal whose segments do not frame to exactly the committed end, is a
    // different thing and is still refused: that is a file with a piece missing from the middle of
    // what it claims, and reading it would give a world with holes in it that says it is finished.
    if (blob.size() > kHeaderBytes + head.journal_bytes) {
        WS_LOG_INFO("cache",
                    "'{}' carries {} bytes past the end of what it committed to -- an append that "
                    "did not finish. Reading the {} bytes it did commit; the next save writes over "
                    "the rest",
                    path, blob.size() - (kHeaderBytes + head.journal_bytes),
                    kHeaderBytes + head.journal_bytes);
    }
    std::vector<SegmentSpan> segments;
    if (!walk_journal(blob.data(), blob.size(), head, segments)) {
        WS_LOG_WARN("cache", "'{}' does not hold the {} segments its header claims; not loading it",
                    path, head.segments);
        return false;
    }
    if (segments.empty() || segments[0].kind != kSegmentFull) return false;

    cache.mode = static_cast<WorldCacheMode>(head.mode);
    cache.baseline_agreed = true;
    cache.baseline_chunks_differing = 0;
    // Refused rather than muddled through. A difference applied over nothing is a building
    // reduced to the holes cut in it -- which looks so much like a world that is still loading
    // that it would be argued about for an hour before anybody suspected the file.
    if (cache.mode == WorldCacheMode::EditOnly && cache.baseline == nullptr) {
        WS_LOG_WARN("cache",
                    "'{}' is a world's difference from its clip and no clip-built world was "
                    "given to lay it over; not loading it",
                    path);
        return false;
    }

    // ---- pass one: what the journal finally says ------------------------------------------
    //
    // Where the last metadata block is, which region blocks are live, and which segment each
    // chunk's bricks came from. Applied rather than replayed, because replaying would intern the
    // tags once per segment and add the ledger's totals up again every time.
    const u8* meta_at = nullptr;
    usize meta_size = 0;
    u32 region_count = 0;
    std::vector<const u8*> block_at;
    std::vector<usize> block_size;
    std::vector<u32> block_len;

    struct ChunkSrc {
        const u8* data = nullptr;
        usize size = 0;
        u32 bricks = 0;
        const u8* clears = nullptr;
        u32 clear_count = 0;
        const u8* erased = nullptr;   // R12d: brick slots a person emptied; two bytes each
        u32 erased_count = 0;
    };
    std::unordered_map<ChunkCoord, ChunkSrc, ChunkCoordHash> live;
    std::vector<ChunkCoord> emptied;

    // Refused rather than believed: the count is four bytes out of a file, and the block vectors
    // below are sized from it. A leaf is 81 bytes and the base segment carries every one of them,
    // so a list that could not fit in the file is not a list.
    const auto set_region_count = [&](u32 count) {
        if (static_cast<u64>(count) * 81 > blob.size()) return false;
        region_count = count;
        const u32 want = region_blocks_for(count);
        block_at.resize(want, nullptr);
        block_size.resize(want, 0);
        block_len.resize(want, 0);
        return true;
    };

    // Walking one chunk's bricks to find where the record ends, without decoding any of them.
    const auto span_of_bricks = [&](Cursor& in, u32 bricks, const u8*& data, usize& bytes) -> bool {
        const usize begin = in.at;
        for (u32 b = 0; b < bricks; ++b) {
            in.take(sizeof(u16));
            if (!in.ok) return false;
            const usize span = brick_span(in.data + in.at, in.size - in.at);
            if (span == 0) return false;
            in.take(span);
            if (!in.ok) return false;
        }
        data = in.data + begin;
        bytes = in.at - begin;
        return true;
    };

    // The edit-only parts, filled in below and used after the baseline has been laid down.
    std::vector<ChunkCoord> expect;
    std::vector<u64> expect_hash;
    const u8* gone_at = nullptr;
    usize gone_size = 0;
    const u8* changed_at = nullptr;
    usize changed_size = 0;

    for (const SegmentSpan& segment : segments) {
        if (segment.kind == kSegmentDirectory) continue;   // the writer's note to the next writer
        Cursor in{segment.data, segment.size, 0, true};

        if (cache.mode == WorldCacheMode::EditOnly) {
            if (segment.kind != kSegmentFull) return false;
            meta_at = take_part(in, meta_size);
            if (meta_at == nullptr) return false;
            usize bytes = 0;
            const u8* regions = take_part(in, bytes);
            if (regions == nullptr) return false;
            {
                Cursor r{regions, bytes, 0, true};
                const u32 count = r.pod<u32>();
                if (!r.ok) return false;
                if (!set_region_count(count)) return false;
                for (u32 block = 0; block < static_cast<u32>(block_at.size()); ++block) {
                    const u32 from = block * kRegionsPerBlock;
                    const u32 to = std::min<u32>(from + kRegionsPerBlock, count);
                    block_at[block] = regions + r.at;
                    block_len[block] = to - from;
                    for (u32 i = from; i < to; ++i) {
                        CachedRegion ignored;
                        if (!read_region(r, ignored)) return false;
                    }
                    block_size[block] = static_cast<usize>(regions + r.at - block_at[block]);
                }
            }
            const u8* edits = take_part(in, bytes);
            if (edits == nullptr) return false;
            {
                Cursor e{edits, bytes, 0, true};
                cache.edits_named = e.pod<u8>() != 0u;
                const u32 boxes = e.pod<u32>();
                if (!e.ok || !e.room_for(boxes, 48)) return false;
                cache.edited.clear();
                cache.edited.reserve(boxes);
                for (u32 i = 0; i < boxes && e.ok; ++i) {
                    CachedEditBox box;
                    for (i64& v : box.low) v = e.pod<i64>();
                    for (i64& v : box.high) v = e.pod<i64>();
                    cache.edited.push_back(box);
                }
                if (!e.ok) return false;
            }
            const u8* fingerprint = take_part(in, bytes);
            if (fingerprint == nullptr) return false;
            {
                Cursor f{fingerprint, bytes, 0, true};
                const u32 count = f.pod<u32>();
                if (!f.ok || !f.room_for(count, 32)) return false;
                expect.resize(count);
                expect_hash.resize(count);
                for (u32 i = 0; i < count && f.ok; ++i) {
                    expect[i].x = f.pod<i64>();
                    expect[i].y = f.pod<i64>();
                    expect[i].z = f.pod<i64>();
                    expect_hash[i] = f.pod<u64>();
                }
                if (!f.ok) return false;
            }
            gone_at = take_part(in, gone_size);
            if (gone_at == nullptr) return false;
            changed_at = take_part(in, changed_size);
            if (changed_at == nullptr) return false;
            continue;
        }

        // ---- a whole world, and the increments over it --------------------------------------
        if (segment.kind != kSegmentFull && segment.kind != kSegmentIncrement) return false;
        usize bytes = 0;
        const u8* block = take_part(in, bytes);
        if (block == nullptr) return false;
        if (bytes > 0) {
            meta_at = block;
            meta_size = bytes;
        } else if (segment.kind == kSegmentFull) {
            return false;   // a whole world with no type table in it is not a world
        }

        const u8* regions = take_part(in, bytes);
        if (regions == nullptr) return false;
        if (segment.kind == kSegmentFull) {
            Cursor r{regions, bytes, 0, true};
            const u32 count = r.pod<u32>();
            if (!r.ok) return false;
            if (!set_region_count(count)) return false;
            for (u32 index = 0; index < static_cast<u32>(block_at.size()); ++index) {
                const u32 from = index * kRegionsPerBlock;
                const u32 to = std::min<u32>(from + kRegionsPerBlock, count);
                block_at[index] = regions + r.at;
                block_len[index] = to - from;
                for (u32 i = from; i < to; ++i) {
                    CachedRegion ignored;
                    if (!read_region(r, ignored)) return false;
                }
                block_size[index] = static_cast<usize>(regions + r.at - block_at[index]);
            }
        } else if (bytes > 0) {
            Cursor r{regions, bytes, 0, true};
            const u32 count = r.pod<u32>();
            const u32 moved = r.pod<u32>();
            if (!r.ok) return false;
            if (!set_region_count(count)) return false;
            for (u32 i = 0; i < moved; ++i) {
                const u32 index = r.pod<u32>();
                const u32 length = r.pod<u32>();
                if (!r.ok) return false;
                usize span = 0;
                const u8* at = take_part(r, span);
                if (at == nullptr) return false;
                if (index >= block_at.size()) return false;
                block_at[index] = at;
                block_size[index] = span;
                block_len[index] = length;
            }
        }

        const u8* chunks = take_part(in, bytes);
        if (chunks == nullptr) return false;
        {
            Cursor c{chunks, bytes, 0, true};
            const u32 count = c.pod<u32>();
            if (!c.ok) return false;
            for (u32 i = 0; i < count; ++i) {
                ChunkCoord coord;
                coord.x = c.pod<i64>();
                coord.y = c.pod<i64>();
                coord.z = c.pod<i64>();
                const u32 bricks = c.pod<u32>();
                if (!c.ok) return false;
                ChunkSrc src;
                src.bricks = bricks;
                if (!span_of_bricks(c, bricks, src.data, src.size)) return false;
                // R12d, and it frames the rest of the record: the erased list is the last thing in
                // a chunk and the next chunk begins after it.
                src.erased_count = c.pod<u32>();
                if (!c.ok) return false;
                src.erased = c.take(src.erased_count * sizeof(u16));
                if (!c.ok) return false;
                live[coord] = src;
            }
        }

        if (segment.kind == kSegmentIncrement) {
            const u8* dropped = take_part(in, bytes);
            if (dropped == nullptr) return false;
            Cursor d{dropped, bytes, 0, true};
            const u32 count = d.pod<u32>();
            if (!d.ok) return false;
            for (u32 i = 0; i < count && d.ok; ++i) {
                ChunkCoord coord;
                coord.x = d.pod<i64>();
                coord.y = d.pod<i64>();
                coord.z = d.pod<i64>();
                if (!d.ok) return false;
                live.erase(coord);
                emptied.push_back(coord);
            }
            if (!d.ok) return false;
        }
    }

    if (meta_at == nullptr) return false;

    // ---- pass two: put it where it goes ----------------------------------------------------
    MetaIn meta;
    {
        Cursor in{meta_at, meta_size, 0, true};
        if (!read_meta_block(in, cache, meta)) return false;
    }

    // ---- R11f data-loss case 2: the baseline's ids and the file's ids ----------------------
    //
    // `adopt` REPLACES the type table, and in edit-only mode that table is not the only thing in
    // the room. The baseline is a world the reading run built for itself, out of the reading run's
    // own table, and every voxel in it is an id into THAT table. Adopt a different one over the
    // top and nothing fails: every voxel still has an id, every id still names a record, and the
    // whole building comes back wearing somebody else's materials -- the marble floor as brick,
    // the glass as lead -- with no warning anywhere.
    //
    // It is not a hypothetical ordering either. A type id is the order things were interned in,
    // interning is driven by the order the sampler meets materials, and the sampler meets them in
    // the order the camera asked for boxes. Two runs of one clip from two cameras can hand back
    // the same world with the ids permuted, which is the case this reader was written assuming
    // could not happen.
    //
    // So the file's table is authoritative -- its brick payloads are written in its own ids -- and
    // the BASELINE is moved to meet it. Records the file has no equivalent for are appended rather
    // than dropped, because a clip that has grown a material since the file was written is the
    // ordinary reason for the tables to differ, and dropping it would take the new matter's colour
    // with it.
    std::vector<VoxelTypeId> type_remap;
    bool types_moved = false;
    if (cache.mode == WorldCacheMode::EditOnly && cache.types->type_count() > 0) {
        const VoxelTypeTable& mine = *cache.types;
        const u32 old_count = mine.type_count();
        const auto file_visual = [&](const VoxelType& t) -> const VisualRecord* {
            return (t.visual < meta.visuals.size()) ? &meta.visuals[t.visual] : nullptr;
        };
        const auto file_behaviour = [&](const VoxelType& t) -> const BehaviourRecord* {
            return (t.behaviour < meta.behaviours.size()) ? &meta.behaviours[t.behaviour] : nullptr;
        };

        bool identical = old_count <= meta.types.size();
        for (u32 i = 0; identical && i < old_count; ++i) {
            const VisualRecord* v = file_visual(meta.types[i]);
            const BehaviourRecord* b = file_behaviour(meta.types[i]);
            identical = v != nullptr && b != nullptr && *v == mine.visual_of(i) &&
                        *b == mine.behaviour_of(i);
        }

        if (!identical) {
            std::unordered_map<u64, std::vector<u32>> visual_by_hash;
            std::unordered_map<u64, std::vector<u32>> behaviour_by_hash;
            for (u32 i = 0; i < static_cast<u32>(meta.visuals.size()); ++i) {
                visual_by_hash[meta.visuals[i].content_hash()].push_back(i);
            }
            for (u32 i = 0; i < static_cast<u32>(meta.behaviours.size()); ++i) {
                behaviour_by_hash[meta.behaviours[i].content_hash()].push_back(i);
            }
            std::unordered_map<u64, u32> type_by_pair;
            type_by_pair.reserve(meta.types.size() * 2 + 1);
            for (u32 i = 0; i < static_cast<u32>(meta.types.size()); ++i) {
                type_by_pair.emplace(
                    (static_cast<u64>(meta.types[i].visual) << 32) | meta.types[i].behaviour, i);
            }

            type_remap.resize(old_count);
            for (u32 i = 0; i < old_count; ++i) {
                const VisualRecord& want_visual = mine.visual_of(i);
                const BehaviourRecord& want_behaviour = mine.behaviour_of(i);
                u32 visual_id = kAirVisual;
                bool found = false;
                for (u32 candidate : visual_by_hash[want_visual.content_hash()]) {
                    if (meta.visuals[candidate] == want_visual) {
                        visual_id = candidate;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    visual_id = static_cast<u32>(meta.visuals.size());
                    meta.visuals.push_back(want_visual);
                    visual_by_hash[want_visual.content_hash()].push_back(visual_id);
                }
                u32 behaviour_id = kAirBehaviour;
                found = false;
                for (u32 candidate : behaviour_by_hash[want_behaviour.content_hash()]) {
                    if (meta.behaviours[candidate] == want_behaviour) {
                        behaviour_id = candidate;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    behaviour_id = static_cast<u32>(meta.behaviours.size());
                    meta.behaviours.push_back(want_behaviour);
                    behaviour_by_hash[want_behaviour.content_hash()].push_back(behaviour_id);
                }
                const u64 pair = (static_cast<u64>(visual_id) << 32) | behaviour_id;
                auto slot = type_by_pair.find(pair);
                if (slot == type_by_pair.end()) {
                    const u32 fresh = static_cast<u32>(meta.types.size());
                    meta.types.push_back(VoxelType{visual_id, behaviour_id});
                    slot = type_by_pair.emplace(pair, fresh).first;
                }
                type_remap[i] = slot->second;
                if (type_remap[i] != i) types_moved = true;
            }
            if (types_moved) {
                WS_LOG_WARN("cache",
                            "'{}' was written against a type table interned in another order than "
                            "this run's; moving the clip's world onto the file's {} types before "
                            "laying the edits over it",
                            path, meta.types.size());
            }
        }
    }

    cache.types->adopt(std::move(meta.visuals), std::move(meta.behaviours), std::move(meta.types));
    cache.materials = std::move(meta.materials);
    cache.stipple_taken = meta.stipple_taken;
    cache.stipple = std::move(meta.stipple);
    cache.emitters = std::move(meta.emitters);

    // The region list, block by block. A block nobody restated is still the one the base segment
    // laid down, which is the whole of what makes an increment cheap.
    cache.regions.clear();
    cache.regions.reserve(region_count);
    for (u32 index = 0; index < static_cast<u32>(block_at.size()); ++index) {
        if (block_at[index] == nullptr) return false;
        const u32 from = index * kRegionsPerBlock;
        const u32 to = std::min<u32>(from + kRegionsPerBlock, region_count);
        if (block_len[index] != to - from) return false;
        Cursor r{block_at[index], block_size[index], 0, true};
        for (u32 i = from; i < to; ++i) {
            CachedRegion region;
            if (!read_region(r, region)) return false;
            cache.regions.push_back(region);
        }
    }
    if (cache.regions.size() != region_count) return false;

    // The totals in the file are the WHOLE world's, taken after the edits, in both modes. On the
    // edit-only path the caller has already built the clip's world into the baseline and charged
    // its ledger for every voxel of it, so adding these on top would count the building twice and
    // the audit would report matter nobody created. Reset and restate. This clears the per-player
    // accounting with it, which is correct here and only here: this is a load, and a load is
    // exactly the moment a ledger has no history worth keeping.
    if (cache.mode == WorldCacheMode::EditOnly && cache.ledger != nullptr) cache.ledger->clear();
    for (const auto& [type, total] : meta.ledger) {
        if (cache.ledger != nullptr && total > 0) {
            cache.ledger->record_bulk(kAir, type, static_cast<u64>(total), MatterReason::Load);
        }
    }

    // Chunks. Created on this thread — the world's map is not safe to insert into from several —
    // and then filled in parallel, because once a chunk exists it is an independent object.
    struct Pending {
        Chunk* chunk = nullptr;
        ChunkCoord coord;
        const u8* clears = nullptr;   // brick slots to empty; edit-only, null in the whole form
        u32 clear_count = 0;
        const u8* data = nullptr;
        usize size = 0;
        u32 bricks = 0;
        const u8* erased = nullptr;   // R12d: slots a person emptied; whole-world form
        u32 erased_count = 0;
    };
    std::vector<Pending> pending;

    // Every brick of a chunk taken back to air, through the route that unlinks it. Not a
    // convenience: a brick left allocated with nothing in it is `world_has` claiming matter the
    // world does not have, which the marcher draws as a filled cube it can never build (D620).
    const auto strip_chunk = [](Chunk& chunk) {
        const u32 axis = static_cast<u32>(kChunkBricks);
        for (u32 bz = 0; bz < axis; ++bz) {
            for (u32 by = 0; by < axis; ++by) {
                for (u32 bx = 0; bx < axis; ++bx) {
                    if (chunk.brick(bx, by, bz) == nullptr) continue;
                    chunk.brick_for_write(bx, by, bz).fill(kAir);
                    chunk.drop_brick_if_empty(bx, by, bz);
                }
            }
        }
        chunk.mark_modified();
    };

    if (cache.mode == WorldCacheMode::EditOnly) {
        // THE BASELINE BECOMES THE WORLD FIRST, and then it is moved onto the file's type table
        // if the two were interned in different orders -- both before a single hash is compared.
        //
        // The order matters and it used to be the other way round. A permuted type table changes
        // every chunk hash in the baseline, so the check would report the whole world in
        // disagreement and blame the clip for a difference that is only a numbering.
        if (cache.baseline != cache.world) {
            cache.baseline->for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
                if (chunk.empty()) return;
                cache.world->chunk_for_write(coord) = chunk;
            });
        }
        if (types_moved) remap_world_types(*cache.world, type_remap, jobs);

        // A hash per chunk is every voxel of the baseline read once, so it goes wide when there
        // is a pool to go wide on. One byte of answer per chunk rather than an atomic: the ranges
        // are disjoint and the sum is trivial afterwards.
        const u64 verify_began = now_ns();
        const u32 fingerprint = static_cast<u32>(expect.size());
        std::vector<u8> disagrees(fingerprint, 0u);
        const auto verify = [&](usize from, usize to) {
            for (usize i = from; i < to; ++i) {
                disagrees[i] = (cache.world->chunk_hash(expect[i]) != expect_hash[i]) ? 1u : 0u;
            }
        };
        if (jobs != nullptr && fingerprint > 1) {
            jobs->parallel_for(fingerprint, 1, verify);
        } else {
            verify(0, fingerprint);
        }
        u32 differing = 0;
        for (u8 flag : disagrees) differing += flag;

        // And the other direction, which the hashes above cannot see: matter in the baseline that
        // the file never knew about. It would survive into the loaded world as if somebody had
        // built it, so it counts as a disagreement even though every named chunk agreed.
        std::unordered_map<ChunkCoord, u8, ChunkCoordHash> named;
        named.reserve(static_cast<usize>(fingerprint) * 2 + 1);
        for (const ChunkCoord& coord : expect) named.emplace(coord, static_cast<u8>(0));
        u32 unexpected = 0;
        cache.world->for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
            if (chunk.empty()) return;
            if (named.find(coord) == named.end()) ++unexpected;
        });
        cache.baseline_chunks_differing = differing + unexpected;
        cache.baseline_agreed = cache.baseline_chunks_differing == 0;
        if (!cache.baseline_agreed) {
            // Said and then gone ahead with. Refusing here hands the caller nothing, and a caller
            // with nothing rebuilds from the clip -- which loses every carving outright, where
            // going ahead loses at most the bricks the file judged identical to a clip that has
            // since moved. See WorldCache::baseline_agreed.
            WS_LOG_WARN("cache",
                        "'{}' was cut from a different world than the one it is being laid over: "
                        "{} of {} chunks disagree ({} the file never saw). The edits it names are "
                        "applied; everything it left to the clip is whatever the clip builds now",
                        path, cache.baseline_chunks_differing, fingerprint, unexpected);
        }
        WS_LOG_INFO("cache", "checked the clip's world against '{}' in {:.0f} ms ({} chunks)", path,
                    ns_to_ms(now_ns() - verify_began), fingerprint);

        // Chunks the clip fills and the saved world does not.
        {
            Cursor g{gone_at, gone_size, 0, true};
            const u32 count = g.pod<u32>();
            if (!g.ok) return false;
            for (u32 i = 0; i < count && g.ok; ++i) {
                ChunkCoord coord;
                coord.x = g.pod<i64>();
                coord.y = g.pod<i64>();
                coord.z = g.pod<i64>();
                if (!g.ok) return false;
                if (!cache.world->has_chunk(coord)) continue;
                strip_chunk(cache.world->chunk_for_write(coord));
                emptied.push_back(coord);
            }
            if (!g.ok) return false;
        }

        Cursor c{changed_at, changed_size, 0, true};
        const u32 changed = c.pod<u32>();
        if (!c.ok) return false;
        pending.reserve(changed);
        for (u32 i = 0; i < changed; ++i) {
            ChunkCoord coord;
            coord.x = c.pod<i64>();
            coord.y = c.pod<i64>();
            coord.z = c.pod<i64>();
            const u32 clear_count = c.pod<u32>();
            if (!c.ok) return false;
            const u8* clears = c.take(clear_count * sizeof(u16));
            if (!c.ok) return false;
            const u32 bricks = c.pod<u32>();
            if (!c.ok) return false;
            const u8* data = nullptr;
            usize bytes = 0;
            if (!span_of_bricks(c, bricks, data, bytes)) return false;
            pending.push_back({&cache.world->chunk_for_write(coord), coord, clears, clear_count,
                               data, bytes, bricks});
        }
    } else {
        // A whole-world file says nothing at all about what was edited, which is not the same as
        // saying nothing was. Trap 7, one more time: leave the claim unmade.
        cache.edits_named = false;
        cache.edited.clear();

        // A chunk the journal dropped after writing it. The world being read into is normally
        // empty, so there is nothing to undo; when it is not, the chunk is stripped rather than
        // left holding what an earlier segment put there.
        for (const ChunkCoord& coord : emptied) {
            if (!cache.world->has_chunk(coord)) continue;
            strip_chunk(cache.world->chunk_for_write(coord));
        }

        std::vector<ChunkCoord> order;
        order.reserve(live.size());
        for (const auto& [coord, src] : live) order.push_back(coord);
        std::sort(order.begin(), order.end(), chunk_coord_less);
        pending.reserve(order.size());
        for (const ChunkCoord& coord : order) {
            const ChunkSrc& src = live[coord];
            pending.push_back({&cache.world->chunk_for_write(coord), coord, nullptr, 0, src.data,
                               src.size, src.bricks, src.erased, src.erased_count});
        }
    }

    const auto fill = [&](usize from, usize to) {
        for (usize i = from; i < to; ++i) {
            Pending& job = pending[i];
            // Clearings first. A brick is named here because it is air in the saved world and the
            // clip puts something there -- the carve -- so it has to go before anything else in
            // this chunk is judged, and it has to go through drop_brick_if_empty (D620).
            for (u32 c = 0; c < job.clear_count; ++c) {
                u16 slot = 0;
                std::memcpy(&slot, job.clears + c * sizeof(u16), sizeof(slot));
                const u32 bx = (slot >> 10) & 31u;
                const u32 by = (slot >> 5) & 31u;
                const u32 bz = slot & 31u;
                if (job.chunk->brick(bx, by, bz) != nullptr) {
                    job.chunk->brick_for_write(bx, by, bz).fill(kAir);
                    job.chunk->drop_brick_if_empty(bx, by, bz);
                }
                // R12d, and AFTER the brick has gone, so the chunk never holds a slot that is both
                // live and erased — which `validate` refuses, and rightly: it would be the two
                // records of the same fact disagreeing.
                //
                // A clearing in an edit-only file IS a carve, by construction: it is there because
                // the clip puts matter where the saved world has none, or because a named edit box
                // reached it. So the slot comes back knowing a person emptied it, which is what
                // stops the next re-sample filling the doorway back in.
                if (edit_tracking()) job.chunk->mark_brick_erased(bx, by, bz);
            }
            usize at = 0;
            for (u32 b = 0; b < job.bricks; ++b) {
                u16 slot = 0;
                std::memcpy(&slot, job.data + at, sizeof(slot));
                at += sizeof(slot);
                const u32 bx = (slot >> 10) & 31u;
                const u32 by = (slot >> 5) & 31u;
                const u32 bz = slot & 31u;
                // Read before `at` moves, and asked of the FILE rather than of the brick: the flag
                // has to go in through `brick_for_write`, because the chunk is what keeps the count
                // and a flag set behind its back is a second index nobody maintains.
                const WriteOrigin origin = (edit_tracking() && brick_was_edited(job.data + at))
                                               ? WriteOrigin::Edit
                                               : WriteOrigin::Field;
                at += read_brick_raw(job.data + at, job.size - at,
                                     job.chunk->brick_for_write(bx, by, bz, origin));
            }
            // The slots a person emptied, in the whole-world form. There is no brick to carry the
            // flag, so the chunk carries the slot.
            for (u32 e = 0; e < job.erased_count; ++e) {
                u16 slot = 0;
                std::memcpy(&slot, job.erased + e * sizeof(u16), sizeof(slot));
                job.chunk->mark_brick_erased((slot >> 10) & 31u, (slot >> 5) & 31u, slot & 31u);
            }
            job.chunk->mark_modified();
        }
    };
    if (jobs != nullptr && pending.size() > 1) {
        jobs->parallel_for(pending.size(), 1, fill);
    } else {
        fill(0, pending.size());
    }

    // A chunk whose last brick went with an edit has to leave the map, for the reason above one
    // level up: `NodePool::world_has` answers above level 8 out of which chunks EXIST, so a chunk
    // emptied by a carve goes on claiming occupancy for the rest of the run (D344's phantom). On
    // this thread, because it erases from the world's map.
    for (const Pending& job : pending) cache.world->drop_chunk_if_empty(job.coord);
    for (const ChunkCoord& coord : emptied) cache.world->drop_chunk_if_empty(coord);
    return true;
}

}  // namespace ws
