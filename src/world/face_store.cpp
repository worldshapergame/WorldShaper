#include "world/face_store.hpp"

#include <algorithm>

namespace ws {

namespace {

u32 next_power_of_two(u32 value) {
    u32 result = 1;
    while (result < value) result <<= 1;
    return result;
}

}  // namespace

void FaceStore::create(const FaceStoreBudget& budget) {
    budget_ = budget;
    faces_.assign(budget_.max_faces, GpuFace{});
    last_read_.assign(budget_.max_faces, 0);
    // Twice the face count, so the table is at most half full and a probe stays short. Open
    // addressing degrades sharply past that, and the table is four bytes an entry against
    // thirty-two for the record it points at, so the headroom is cheap.
    entries_.assign(next_power_of_two(std::max<u32>(budget_.max_faces * 2, 1024)), kNoFace);
    dirty_faces_.create(budget_.max_faces);
    dirty_entries_.create(entries_.size());
    // The one array whose empty value is not zero: a free bucket is kNoFace, which is all ones,
    // and a device buffer starts at all zeroes. The node pool learned this the hard way when the
    // audit named byte 0 of its entry table (D236).
    dirty_entries_.mark_range(0, entries_.size());
    free_faces_.clear();
    next_free_ = 0;
    out_of_room_ = false;
    claims_ = 0;
    hits_ = 0;
    evictions_ = 0;
}

usize FaceStore::bucket_of(const FaceKey& key) const {
    return static_cast<usize>(FaceKeyHash{}(key)) & (entries_.size() - 1);
}

u32 FaceStore::find(const FaceKey& key) const {
    if (entries_.empty()) return kNoFace;
    const usize capacity = entries_.size();
    const usize bucket = bucket_of(key);
    for (usize probe = 0; probe < capacity; ++probe) {
        const u32 slot = entries_[(bucket + probe) & (capacity - 1)];
        // An empty bucket ends the run. A probe that walked past one would miss a face that a
        // later insertion displaced.
        if (slot == kNoFace) return kNoFace;
        // A tombstone does NOT end it -- that is what makes it a tombstone rather than a hole --
        // and it is not a slot number either. Indexing it read faces_[0xFFFFFFFE] and took the
        // process down, which is the test for eviction finding a crash in the lookup.
        if (slot == kFaceTombstone) continue;
        const GpuFace& face = faces_[slot];
        if (face.x == static_cast<i32>(key.x) && face.y == static_cast<i32>(key.y) &&
            face.z == static_cast<i32>(key.z) && face_level(face) == key.level &&
            face_direction(face) == key.face) {
            return slot;
        }
    }
    return kNoFace;
}

u32 FaceStore::claim(const FaceKey& key, u64 frame) {
    ++claims_;
    const u32 existing = find(key);
    if (existing != kNoFace) {
        ++hits_;
        last_read_[existing] = static_cast<u32>(frame);
        return existing;
    }

    u32 slot = kNoFace;
    if (!free_faces_.empty()) {
        slot = free_faces_.back();
        free_faces_.pop_back();
    } else if (next_free_ < budget_.max_faces) {
        slot = next_free_++;
    } else {
        // Full is a fact about this table, never about the world. Saying so is what lets a caller
        // tell "there is no face here" from "I could not fit one", which is the distinction the
        // node pool needed `out_of_room_` for and the one whose absence made a tree that ran out
        // of memory look like a tree over empty space.
        out_of_room_ = true;
        return kNoFace;
    }

    faces_[slot] = GpuFace{};
    faces_[slot].x = static_cast<i32>(key.x);
    faces_[slot].y = static_cast<i32>(key.y);
    faces_[slot].z = static_cast<i32>(key.z);
    faces_[slot].packed = pack_face(key.level, key.face, 0);
    faces_[slot].bins = kNoOffset;
    last_read_[slot] = static_cast<u32>(frame);
    dirty_faces_.mark(slot);

    const usize capacity = entries_.size();
    const usize bucket = bucket_of(key);
    for (usize probe = 0; probe < capacity; ++probe) {
        u32& cell = entries_[(bucket + probe) & (capacity - 1)];
        // A tombstone is free to take: `find` already ran to completion above and said this key
        // is not in the table, so nothing behind it is this key.
        if (cell == kNoFace || cell == kFaceTombstone) {
            cell = slot;
            dirty_entries_.mark((bucket + probe) & (capacity - 1));
            return slot;
        }
    }

    // The table is sized at twice the face count, so reaching here means every bucket is taken
    // while a face slot was free, which cannot happen unless one of the two is mis-sized.
    free_faces_.push_back(slot);
    out_of_room_ = true;
    return kNoFace;
}

void FaceStore::touch(u32 slot, u64 frame) {
    if (slot < last_read_.size()) last_read_[slot] = static_cast<u32>(frame);
}

void FaceStore::write(u32 slot, u32 irradiance, u32 samples, u32 lit) {
    if (slot >= faces_.size()) return;
    GpuFace& face = faces_[slot];
    face.irradiance = irradiance;
    face.counters = pack_counters(samples, lit);
    dirty_faces_.mark(slot);
}

void FaceStore::evict_cold(u64 frame) {
    const u32 now = static_cast<u32>(frame);
    for (u32 slot = 0; slot < next_free_; ++slot) {
        // The cheap test first and the record second, which on the node pool was the difference
        // between 8.6 MB of memory traffic a frame and one megabyte (D273).
        if (now - last_read_[slot] <= budget_.cold_frames) continue;
        if (face_level(faces_[slot]) == 0 && faces_[slot].packed == 0) continue;   // already free

        const FaceKey key{faces_[slot].x, faces_[slot].y, faces_[slot].z,
                          face_level(faces_[slot]), face_direction(faces_[slot])};
        const usize capacity = entries_.size();
        const usize bucket = bucket_of(key);
        for (usize probe = 0; probe < capacity; ++probe) {
            u32& cell = entries_[(bucket + probe) & (capacity - 1)];
            if (cell == kNoFace) break;
            if (cell == slot) {
                // Left as a tombstone rather than emptied, because emptying a bucket mid-run cuts
                // every face behind it out of its own probe sequence -- they are still in the
                // table and can no longer be found, which is a stale-but-unreachable entry and
                // the worst of both.
                cell = kFaceTombstone;
                dirty_entries_.mark((bucket + probe) & (capacity - 1));
                break;
            }
        }
        faces_[slot] = GpuFace{};
        dirty_faces_.mark(slot);
        last_read_[slot] = now;
        free_faces_.push_back(slot);
        ++evictions_;
    }
}

FaceStoreStats FaceStore::stats() const {
    FaceStoreStats s;
    s.faces = next_free_ - static_cast<u32>(free_faces_.size());
    s.face_bytes = static_cast<u64>(s.faces) * sizeof(GpuFace);
    s.total_bytes = s.face_bytes + entries_.size() * sizeof(u32);
    s.claims = claims_;
    s.hits = hits_;
    s.evictions = evictions_;
    return s;
}

bool FaceStore::validate() const {
    // Every published bucket points at a live face, and every live face is findable by its own
    // key. The second half is the one that matters: a face that is in the table and cannot be
    // found is invisible to the pass that would refresh it and is never noticed.
    for (const u32 slot : entries_) {
        if (slot == kNoFace || slot == kFaceTombstone) continue;
        if (slot >= next_free_) return false;
    }
    for (u32 slot = 0; slot < next_free_; ++slot) {
        const GpuFace& face = faces_[slot];
        if (face.packed == 0 && face.x == 0 && face.y == 0 && face.z == 0) continue;
        const FaceKey key{face.x, face.y, face.z, face_level(face), face_direction(face)};
        if (find(key) != slot) return false;
    }
    return true;
}

}  // namespace ws
