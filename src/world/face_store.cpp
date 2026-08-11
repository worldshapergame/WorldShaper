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
    secondary_.assign(budget_.max_faces, 0);
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
    evict_cursor_ = 0;
    last_emergency_ = 0;
    out_of_room_ = false;
    claims_ = 0;
    hits_ = 0;
    evictions_ = 0;
    refusals_ = 0;
    secondary_live_ = 0;
    secondary_declined_ = 0;
    promotions_ = 0;
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

u32 FaceStore::secondary_cap() const {
    const u32 share = std::max(2u, budget_.secondary_share);
    return budget_.max_faces / share;
}

void FaceStore::promote(u32 slot) {
    if (slot >= faces_.size() || !is_secondary(slot)) return;
    if (!face_live(faces_[slot])) return;
    secondary_[slot] = 0;
    if (secondary_live_ > 0) --secondary_live_;
    ++promotions_;
    // No dirty mark, and that is the point: nothing about the card's copy of this record changed.
    // See `is_secondary`.
}

u32 FaceStore::claim(const FaceKey& key, u64 frame, bool* was_new, bool secondary) {
    if (was_new != nullptr) *was_new = false;
    ++claims_;
    const u32 existing = find(key);
    if (existing != kNoFace) {
        ++hits_;
        last_read_[existing] = static_cast<u32>(frame);
        // A face already here is never demoted by a ray asking for it again. The class answers "who
        // wants this", and the answer once a pixel has wanted it is the pixel, for as long as the
        // face lives -- a bounce ray landing on the wall in front of you must not put that wall
        // inside the off-screen cap.
        if (!secondary) promote(existing);
        return existing;
    }

    // The cap, and it is checked before a slot is taken rather than after. R9b: a class that
    // overruns degrades its own refresh rate and nothing else's, so this returns kNoFace WITHOUT
    // setting `out_of_room_` and without counting a refusal. The store is not full; this class is.
    //
    // Two tests, and the second is what makes "R9a cannot cause a refusal" structural rather than
    // hopeful. The cap alone bounds the off-screen set against the TABLE; it does not bound the two
    // sets together, and the on-screen set has no bound at all -- flying, it has measured 995,684
    // live faces against a table of 1,048,576 (D504). A quarter of the table held off-screen on top
    // of that is the store full and refusing, which is a visible surface with no light of its own
    // and is the exact picture D502 was reported as. So the moment the store is tight enough to
    // start spending its history, the off-screen set stops growing at all: it is the class whose
    // faces nobody is looking at, and it is the one that can afford to wait.
    if (secondary && (secondary_live_ >= secondary_cap() || pressure_shift() > 0)) {
        ++secondary_declined_;
        return kNoFace;
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
        ++refusals_;
        return kNoFace;
    }

    // After the table has actually found room, so a refused claim reports "not new": the caller's
    // follow-on work is for a face that is now here, and there is no face here.
    if (was_new != nullptr) *was_new = true;

    faces_[slot] = GpuFace{};
    faces_[slot].x = static_cast<i32>(key.x);
    faces_[slot].y = static_cast<i32>(key.y);
    faces_[slot].z = static_cast<i32>(key.z);
    faces_[slot].packed = pack_face(key.level, key.face, kFaceLive);
    secondary_[slot] = secondary ? 1 : 0;
    if (secondary) ++secondary_live_;
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
    if (secondary && secondary_live_ > 0) --secondary_live_;
    secondary_[slot] = 0;
    faces_[slot] = GpuFace{};
    dirty_faces_.mark(slot);
    free_faces_.push_back(slot);
    out_of_room_ = true;
    ++refusals_;
    return kNoFace;
}

u32 FaceStore::min_cold() const {
    // Twice the lattice's period, not once. Once is the average wait for a face that covers many
    // pixels; a face covering a single pixel waits the whole period, and the phase walk is not
    // synchronised with anything here. Two periods is the cheapest margin that makes "not claimed"
    // mean "not wanted" rather than "not asked yet".
    //
    // This quantity ALONE, and `kFaceMinCold` is not part of it -- see that constant for why a floor
    // reasoned from how long a face takes to converge is a floor about the wrong thing.
    return budget_.claim_period * 2;
}

u32 FaceStore::pressure_shift() const {
    if (!budget_.pressure || budget_.max_faces == 0) return 0;
    // Free space is the whole table minus what is live, so the recycled slots on `free_faces_`
    // count as free -- they are exactly what a claim is about to take.
    //
    // Nothing happens at all until the table is half full, which on this building is a camera that
    // has genuinely seen half a million faces. The point is to spend HISTORY before the table is
    // full rather than to keep the store small.
    const u32 used = next_free_ - static_cast<u32>(free_faces_.size());
    const u32 free_now = (used < budget_.max_faces) ? budget_.max_faces - used : 0;
    u32 shift = 0;
    const u32 from = std::max(2u, budget_.pressure_from);
    while (shift < 8 && free_now * (from << shift) <= budget_.max_faces) ++shift;
    return shift;
}

u32 FaceStore::cold_now() const {
    const u32 floor_frames = min_cold();
    const u32 relaxed = std::max(budget_.cold_frames, floor_frames);
    const u32 shift = pressure_shift();
    if (shift == 0) return relaxed;                                // an eighth free or better
    if (shift == 1) return std::max(relaxed >> 2, floor_frames);   // under an eighth
    return floor_frames;                                           // under a sixteenth

    // Three steps rather than a smooth ramp, and the reason is that a window has to BITE.
    //
    // The obvious shape is to halve the window per halving of the free space, and it does not work:
    // a table that fills in sixty-eight frames holds nothing older than sixty-eight frames, so a
    // window of three hundred, or a hundred and fifty, or seventy-five evicts NOTHING however
    // alarming the occupancy is. The store then sails past full with every window above it having
    // done nothing, and refuses claims for the handful of frames it takes the ladder to reach a
    // number smaller than the ages actually present. Those frames are the fault.
    //
    // Under a quarter free there is nothing left worth being gradual about: everything above the
    // floor is history — a face nobody has looked at for longer than the lattice needs to come back
    // — and the alternative to spending it is refusing a face somebody is looking at now.
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
    if (next_free_ == 0) return;
    const u32 now = static_cast<u32>(frame);

    // How cold is cold. Normally the budget's own figure; when the table has run out of slots,
    // whatever it takes to get some back.
    //
    // Without the second half the store never recovers from being full. A face is given up after
    // six hundred frames of nobody asking for it, and a player who fills the table in less than
    // that -- which one camera position now does, since faces became voxels -- gets no new faces
    // at all until ten seconds after they stop moving. What that looks like is the reported bug
    // exactly: shadows do not stop being *drawn*, they stop being *produced*, so the picture keeps
    // whatever set of shadowed faces it had and every new surface is fully lit.
    //
    // Halving down to a floor rather than dropping straight to it, so a store that is only just
    // full gives up only what it must. The floor is well above the handful of frames a face needs
    // to settle, or the store would recycle faces it is still converging.
    // The window this frame, which is the relaxed one until the table starts running out and then
    // shortens with the pressure. Never below `min_cold`, which is the lattice's own period and is
    // the number the old floor of 32 was wrong about. See kFaceMinCold.
    u32 cold = cold_now();
    const u32 floor_frames = min_cold();
    if (out_of_room_ && free_faces_.empty() &&
        (last_emergency_ == 0 || now - last_emergency_ >= kFaceEmergencyGap)) {
        // Rate limited, because this scans the whole table several times over and a store whose
        // every face is genuinely hot would otherwise pay that on every frame for ever -- turning
        // "no new faces" into "no new faces and a frame rate to match". Once every sixty frames is
        // often enough to recover in under a second and rare enough to be invisible if it cannot.
        last_emergency_ = now;
        // Once at whatever the window is, and only then halve. The halving used to come first,
        // which is fine while the window has room above the floor and skips the sweep entirely once
        // pressure has already taken it down to the floor -- a table that is full, has faces older
        // than the floor in it, and gives up none of them.
        sweep(now, cold, 0, next_free_);
        // Down to the floor and no further, and the floor is now the lattice's period rather than
        // 32. The old loop tested `cold > kFaceMinCold` BEFORE halving, so the window it actually
        // reached was 18 -- under a third of the 64 frames the lattice takes to come back, so the
        // sweep that was meant to recover the table was evicting the faces the camera was pointed
        // at.
        while (cold > floor_frames && free_faces_.empty()) {
            cold = std::max(cold >> 1, floor_frames);
            sweep(now, cold, 0, next_free_);
        }
        evict_cursor_ = 0;
        out_of_room_ = free_faces_.empty();
        return;
    }

    // A slice a frame, exactly as the node pool's erosion sweep does it and for the reason D273
    // records: the timestamp array is a quarter the width of a record and read in order, so the
    // cheap test costs a fraction of what looking at every face would. Eight slices costs eight
    // frames of latency on an eviction against a window of six hundred.
    //
    // ...and MORE slices as the table fills, because eight frames of latency is fine against six
    // hundred and is not fine against a table that fills in sixty-eight. Shortening the window
    // without also looking oftener leaves the shorter window unenforced for eight frames, and a
    // claim refused in those eight frames is a surface with no light of its own — which is the whole
    // fault. At full pressure this is the whole table every frame, which is one pass over an array a
    // quarter the width of a record and is only paid by a store that is nearly full.
    // Under real pressure it is the WHOLE table, from nought, and not a bigger slice.
    //
    // A slice is a cursor, and a cursor that happens to be sitting at the young end of a table that
    // is still growing sweeps only the faces it has just claimed — every one of them younger than
    // any window — and reports having found nothing to give up while the table fills behind it.
    // Measured in the test below: window already down to the floor, half the table swept per frame,
    // and nought evictions until the cursor wrapped. Where the cursor is is not a fact about the
    // store's pressure, so it must not decide what the store looks at when it is under pressure.
    if (pressure_shift() >= 2) {
        sweep(now, cold, 0, next_free_);
        evict_cursor_ = 0;
        return;
    }

    const u32 slice = (next_free_ + kFaceEvictSlices - 1) / kFaceEvictSlices;
    if (evict_cursor_ >= next_free_) evict_cursor_ = 0;
    const u32 first = evict_cursor_;
    const u32 last = (first + slice < next_free_) ? first + slice : next_free_;
    evict_cursor_ = last;
    sweep(now, cold, first, last);
}

void FaceStore::sweep(u32 now, u32 cold, u32 first, u32 last) {
    for (u32 slot = first; slot < last; ++slot) {
        // The cheap test first and the record second, which on the node pool was the difference
        // between 8.6 MB of memory traffic a frame and one megabyte (D273).
        if (now - last_read_[slot] <= cold) continue;
        if (!face_live(faces_[slot])) continue;   // already free

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
        if (secondary_[slot] != 0 && secondary_live_ > 0) --secondary_live_;
        secondary_[slot] = 0;
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
    s.refusals = refusals_;
    s.cold_window = cold_now();
    s.secondary = secondary_live_;
    s.secondary_cap = secondary_cap();
    s.secondary_declined = secondary_declined_;
    s.promotions = promotions_;
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
    u32 secondary = 0;
    for (u32 slot = 0; slot < next_free_; ++slot) {
        const GpuFace& face = faces_[slot];
        if (!face_live(face)) continue;
        if (is_secondary(slot)) ++secondary;
        const FaceKey key{face.x, face.y, face.z, face_level(face), face_direction(face)};
        if (find(key) != slot) return false;
    }
    // The class counter against the classes actually present. It is the one number here that is
    // maintained incrementally rather than derived, so it is the one that can drift -- a decrement
    // missed on one of eviction, promotion or the mis-sized-table path would let the off-screen cap
    // creep upwards for the rest of the run with nothing to say so.
    return secondary == secondary_live_;
}

}  // namespace ws
