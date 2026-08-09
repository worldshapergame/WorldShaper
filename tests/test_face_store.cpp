#include <doctest/doctest.h>

#include "world/face_store.hpp"

using namespace ws;

namespace {

FaceStore make_store(u32 max_faces = 1024, u32 cold_frames = 4) {
    FaceStore store;
    FaceStoreBudget budget;
    budget.max_faces = max_faces;
    budget.cold_frames = cold_frames;
    store.create(budget);
    return store;
}

}  // namespace

TEST_CASE("a face is claimed once and found again") {
    FaceStore store = make_store();
    const FaceKey key{4, -9, 22, 3, 2};

    const u32 slot = store.claim(key, 1);
    REQUIRE(slot != kNoFace);
    CHECK(store.claim(key, 1) == slot);   // claiming again is finding
    CHECK(store.find(key) == slot);
    CHECK(store.stats().faces == 1);
    CHECK(store.validate());
}

// Six faces of one node are six different faces. Getting this wrong means a wall lit from the
// south shares its answer with the one facing north, which is not subtle.
TEST_CASE("the six directions of one node are six faces") {
    FaceStore store = make_store();
    u32 slots[kFaceCount]{};
    for (u32 face = 0; face < kFaceCount; ++face) {
        slots[face] = store.claim(FaceKey{1, 2, 3, 3, face}, 1);
        REQUIRE(slots[face] != kNoFace);
    }
    for (u32 a = 0; a < kFaceCount; ++a) {
        for (u32 b = a + 1; b < kFaceCount; ++b) CHECK(slots[a] != slots[b]);
    }
    CHECK(store.stats().faces == kFaceCount);
    CHECK(store.validate());
}

// The same face at two detail levels is two faces, because the composite blends between adjacent
// levels and has to be able to read both (documentation/21 §5).
TEST_CASE("one face at two levels is two faces") {
    FaceStore store = make_store();
    const u32 fine = store.claim(FaceKey{8, 0, 0, 3, 0}, 1);
    const u32 coarse = store.claim(FaceKey{8, 0, 0, 4, 0}, 1);
    REQUIRE(fine != kNoFace);
    REQUIRE(coarse != kNoFace);
    CHECK(fine != coarse);
    CHECK(store.validate());
}

// Negative coordinates are where an arithmetic shift and a division part company, and every
// structure in this engine that got it wrong put the whole negative half of the world one cell
// out. Cheap to assert, and it has caught this before.
TEST_CASE("negative coordinates are their own faces") {
    FaceStore store = make_store();
    const u32 negative = store.claim(FaceKey{-1, -1, -1, 3, 0}, 1);
    const u32 positive = store.claim(FaceKey{1, 1, 1, 3, 0}, 1);
    REQUIRE(negative != kNoFace);
    REQUIRE(positive != kNoFace);
    CHECK(negative != positive);
    CHECK(store.find(FaceKey{-1, -1, -1, 3, 0}) == negative);
    CHECK(store.validate());
}

TEST_CASE("what a shading pass writes is what the composite reads") {
    FaceStore store = make_store();
    const FaceKey key{5, 5, 5, 3, 1};
    const u32 slot = store.claim(key, 1);
    REQUIRE(slot != kNoFace);

    store.write(slot, 0x1234u, 200, 64, 7);
    const GpuFace& face = store.faces()[slot];
    CHECK(face.irradiance == 0x1234u);
    CHECK(face_visibility(face) == 200);
    CHECK(face_samples(face) == 64);
    CHECK(face_variance(face) == 7);
    CHECK(store.validate());
}

// A matte face allocates no payload, which is most of a world (documentation/21 §4).
TEST_CASE("a face with no directional payload carries none") {
    FaceStore store = make_store();
    const u32 slot = store.claim(FaceKey{0, 0, 0, 3, 0}, 1);
    REQUIRE(slot != kNoFace);
    CHECK(store.faces()[slot].bins == kNoOffset);
}

// Full is a fact about the table and never about the world. Conflating the two is what made a
// node pool that had run out of memory look like a pool over empty space (D210).
TEST_CASE("a full table says so rather than answering wrongly") {
    FaceStore store = make_store(8);
    for (u32 i = 0; i < 8; ++i) {
        REQUIRE(store.claim(FaceKey{static_cast<i64>(i), 0, 0, 3, 0}, 1) != kNoFace);
    }
    CHECK_FALSE(store.out_of_room());
    CHECK(store.claim(FaceKey{99, 0, 0, 3, 0}, 1) == kNoFace);
    CHECK(store.out_of_room());
    // And the ones that fitted are still exactly where they were.
    for (u32 i = 0; i < 8; ++i) {
        CHECK(store.find(FaceKey{static_cast<i64>(i), 0, 0, 3, 0}) != kNoFace);
    }
    CHECK(store.validate());
}

TEST_CASE("a face nothing reads is given up, and one that is read is not") {
    FaceStore store = make_store(1024, 4);
    const FaceKey kept{1, 0, 0, 3, 0};
    const FaceKey dropped{2, 0, 0, 3, 0};
    const u32 kept_slot = store.claim(kept, 1);
    REQUIRE(store.claim(dropped, 1) != kNoFace);

    for (u64 frame = 2; frame < 40; ++frame) {
        store.touch(kept_slot, frame);
        store.evict_cold(frame);
    }

    CHECK(store.find(kept) == kept_slot);
    CHECK(store.find(dropped) == kNoFace);
    CHECK(store.validate());
}

// The one that matters about an open-addressed table: emptying a bucket in the middle of a probe
// run cuts everything behind it out of its own sequence. Those faces are still in the table and
// can no longer be found, so the pass that would refresh them never sees them and nothing ever
// says so. Claim a run that collides, drop the first, and the rest must still be findable.
TEST_CASE("evicting one face does not hide the ones behind it") {
    FaceStore store = make_store(1024, 4);
    FaceKey keys[16]{};
    u32 slots[16]{};
    for (u32 i = 0; i < 16; ++i) {
        keys[i] = FaceKey{static_cast<i64>(i), 7, 7, 3, i % kFaceCount};
        slots[i] = store.claim(keys[i], 1);
        REQUIRE(slots[i] != kNoFace);
    }

    // Everything but the first keeps being read.
    for (u64 frame = 2; frame < 40; ++frame) {
        for (u32 i = 1; i < 16; ++i) store.touch(slots[i], frame);
        store.evict_cold(frame);
    }

    CHECK(store.find(keys[0]) == kNoFace);
    for (u32 i = 1; i < 16; ++i) {
        INFO("face " << i);
        CHECK(store.find(keys[i]) == slots[i]);
    }
    CHECK(store.validate());
}

// A dropped slot comes back, and comes back clean: whatever the last face wrote into it must not
// be readable as the new face's light.
TEST_CASE("a reclaimed slot carries nothing of the face before it") {
    FaceStore store = make_store(1024, 4);
    const u32 first = store.claim(FaceKey{3, 3, 3, 3, 0}, 1);
    store.write(first, 0xFFFFFFFFu, 255, 4096, 255);

    for (u64 frame = 2; frame < 40; ++frame) store.evict_cold(frame);
    REQUIRE(store.find(FaceKey{3, 3, 3, 3, 0}) == kNoFace);

    const u32 second = store.claim(FaceKey{4, 4, 4, 3, 1}, 100);
    REQUIRE(second != kNoFace);
    const GpuFace& face = store.faces()[second];
    CHECK(face.irradiance == 0u);
    CHECK(face_samples(face) == 0u);
    CHECK(face_visibility(face) == 0u);
    CHECK(store.validate());
}
