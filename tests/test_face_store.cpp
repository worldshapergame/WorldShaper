#include <doctest/doctest.h>

#include "world/face_store.hpp"

using namespace ws;

namespace {

FaceStore make_store(u32 max_faces = 1024, u32 cold_frames = 4) {
    FaceStore store;
    FaceStoreBudget budget;
    budget.max_faces = max_faces;
    budget.cold_frames = cold_frames;
    // A lattice period of one, so `cold_frames` means what these cases say it means.
    //
    // The store floors every eviction window at twice the period the request lattice takes to come
    // back to a pixel, because below that "nobody has claimed this" means "the lattice has not got
    // round to it" (see kFaceMinCold). That floor is 128 frames at the real default, which would
    // quietly raise every short window below and turn these into tests of nothing. Naming it here
    // is the honest way to say what each case is about: this one is about the cold rule, not about
    // the lattice.
    budget.claim_period = 1;
    store.create(budget);
    return store;
}

}  // namespace

// The failure this pair is written for is not "eviction is wrong", it is "eviction never ran".
//
// `evict_cold` was written with the store and tested with the store, and no caller ever called it.
// A store that only grows reaches its cap and then refuses every face after it, and what that looks
// like from the game is not shadows switching off: the faces that already have light keep it, so
// the picture holds whatever set of shadowed surfaces it had when the table filled and everything
// found after that is lit by the composite's fallback. Nothing in the store is in a bad state and
// no counter is obviously wrong, which is why it took a player to notice.
TEST_CASE("a full store recovers, rather than refusing faces for ever") {
    FaceStore store = make_store(64, 600);

    // Fill it, with every face claimed at frame 1 and never asked for again.
    for (u32 i = 0; i < 64; ++i) {
        REQUIRE(store.claim(FaceKey{i, 0, 0, 0, 0}, 1) != kNoFace);
    }
    CHECK(store.stats().faces == 64);
    CHECK(store.claim(FaceKey{999, 0, 0, 0, 0}, 2) == kNoFace);
    CHECK(store.out_of_room());

    // Well inside cold_frames, so the ordinary rule would give up nothing at all. The store is
    // full and the faces are not cold: it has to take the coldest anyway or the game stops
    // producing shadows until the player stands still for ten seconds.
    store.evict_cold(200);
    const u32 slot = store.claim(FaceKey{999, 0, 0, 0, 0}, 200);
    CHECK(slot != kNoFace);
    CHECK(store.find(FaceKey{999, 0, 0, 0, 0}) == slot);
    CHECK(store.validate());
}

// The fault the pressure rule exists for, written as the thing a player does rather than as an
// invariant: keep looking at new surfaces, never at more of them at once than the table holds.
//
// The old policy waits for the table to be FULL and only then gives anything up, so a visible set
// that fits several times over still fills the table with history — and a refused claim is a pixel
// with no face of its own and no host stand-in either, which falls to the card's provisional coarse
// face and gets ONE fresh sample per frame. That is the blocky picture that changes completely every
// frame. Measured in the game at a forced budget: 231,409 pixels of 1,390,170 differing between two
// consecutive frames of a STATIC camera with no edits at all, against 56,284 with room to spare.
TEST_CASE("a store under pressure spends its history rather than refusing a face") {
    FaceStore store;
    FaceStoreBudget budget;
    budget.max_faces = 4096;
    budget.cold_frames = 600;
    budget.claim_period = 8;    // so the floor is 16 and the window has somewhere to go
    store.create(budget);

    // Two hundred faces are on screen and re-claimed every frame; everything else is walked past
    // once and never asked for again. Six hundred frames of that is far more history than the table
    // can hold, which is the player's case exactly.
    u64 refused = 0;
    u64 first_refusal = 0;
    u32 used_then = 0;
    for (u64 frame = 1; frame <= 600; ++frame) {
        for (u32 i = 0; i < 200; ++i) {
            if (store.claim(FaceKey{i, 0, 0, 0, 0}, frame) == kNoFace) ++refused;
        }
        for (u32 i = 0; i < 60; ++i) {
            const i64 fresh = static_cast<i64>(1000 + frame * 60 + i);
            if (store.claim(FaceKey{fresh, 1, 0, 0, 0}, frame) == kNoFace) ++refused;
        }
        if (refused > 0 && first_refusal == 0) {
            first_refusal = frame;
            used_then = store.stats().faces;
        }
        store.evict_cold(frame);
    }

    // Named rather than counted, because "it refused four faces on frame 65" and "it refuses faces"
    // are the same failing assertion and different bugs.
    CHECK(first_refusal == 0);
    CHECK(used_then == 0);
    CHECK(refused == 0);
    CHECK(store.stats().refusals == 0);
    CHECK_FALSE(store.out_of_room());
    // ...and the two hundred faces being looked at are all still there, with their light. Giving
    // history up is only the right answer while what is on screen survives it.
    for (u32 i = 0; i < 200; ++i) {
        CHECK(store.find(FaceKey{i, 0, 0, 0, 0}) != kNoFace);
    }
    CHECK(store.validate());
}

// The floor, which is the half of it that the old constant got wrong.
TEST_CASE("eviction never uses a window shorter than the request lattice's own period") {
    FaceStore store;
    FaceStoreBudget budget;
    budget.max_faces = 64;
    budget.cold_frames = 600;
    budget.claim_period = 256;   // 4K
    store.create(budget);

    // Full, and every face claimed recently enough that the lattice has not had its turn yet.
    for (u32 i = 0; i < 64; ++i) REQUIRE(store.claim(FaceKey{i, 0, 0, 0, 0}, 1000) != kNoFace);
    CHECK(store.min_cold() == 512);
    CHECK(store.cold_now() >= 512);

    // The emergency sweep runs and finds nothing it is allowed to take. That is the correct answer:
    // a face claimed a hundred frames ago at 4K has not been abandoned, it has not been ASKED yet,
    // and the old floor of 32 took it and handed the picture a slot with no light in it.
    store.claim(FaceKey{999, 0, 0, 0, 0}, 1100);
    store.evict_cold(1100);
    for (u32 i = 0; i < 64; ++i) CHECK(store.find(FaceKey{i, 0, 0, 0, 0}) != kNoFace);
    CHECK(store.validate());
}

// And the ordinary path still gives up only what is genuinely cold, a slice at a time.
TEST_CASE("a face still in use is not evicted, however many sweeps run") {
    FaceStore store = make_store(1024, 8);
    const FaceKey kept{1, 2, 3, 0, 4};
    const FaceKey dropped{9, 9, 9, 0, 4};
    REQUIRE(store.claim(kept, 1) != kNoFace);
    REQUIRE(store.claim(dropped, 1) != kNoFace);

    // kFaceEvictSlices sweeps cover the whole table once, so run several times that many.
    for (u64 frame = 2; frame < 60; ++frame) {
        store.claim(kept, frame);        // asked for every frame
        store.evict_cold(frame);
    }
    CHECK(store.find(kept) != kNoFace);
    CHECK(store.find(dropped) == kNoFace);
    CHECK(store.validate());
}

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

    store.write(slot, 0x1234u, 64, 48);
    const GpuFace& face = store.faces()[slot];
    CHECK(face.irradiance == 0x1234u);
    CHECK(face_samples(face) == 64);
    CHECK(face_lit(face) == 48);
    CHECK(face_visibility(face) == doctest::Approx(0.75));
    CHECK(store.validate());
}

// The counts are counts, not a mean, and this is the property that failed when they were one.
//
// A face lit by every ray must read as fully lit however many rays it has had. Stored as an
// eight-bit running mean it read 245 of 255 after 494 rays and would never have read more, because
// the update rounded back to where it started once the step fell below half a count -- so "always
// lit" and "nearly always lit" became the same number and no test could tell them apart.
TEST_CASE("a face lit by every ray reads as fully lit, at any sample count") {
    FaceStore store = make_store();
    const u32 slot = store.claim(FaceKey{7, 1, 2, 3, 2}, 1);
    REQUIRE(slot != kNoFace);

    for (u32 samples : {1u, 4u, 64u, 494u, 4096u, 65535u}) {
        store.write(slot, 0u, samples, samples);
        CHECK(face_visibility(store.faces()[slot]) == doctest::Approx(1.0));
    }
    // And one in four is one in four at every one of them, rather than drifting with the count.
    for (u32 samples : {4u, 64u, 4096u}) {
        store.write(slot, 0u, samples, samples / 4u);
        CHECK(face_visibility(store.faces()[slot]) == doctest::Approx(0.25));
    }
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
// `was_new` decides whether the caller claims a stand-in beside this face, and the caller runs
// sixteen thousand times a frame — so "new" has to mean *this call put it here*, not "a claim
// happened" and not "the store has one of these". Both of the wrong answers are silent: reporting
// new on a repeat is the cost the gate exists to avoid, and reporting new on a refusal hangs a
// stand-in claim off a face that is not in the table at all.
TEST_CASE("a claim says whether it is what put the face here") {
    FaceStore store = make_store(4, 600);

    bool was_new = false;
    REQUIRE(store.claim(FaceKey{1, 2, 3, 0, 4}, 1, &was_new) != kNoFace);
    CHECK(was_new);

    // The same face again, which is the overwhelming majority of what the marcher reports.
    was_new = true;
    REQUIRE(store.claim(FaceKey{1, 2, 3, 0, 4}, 2, &was_new) != kNoFace);
    CHECK_FALSE(was_new);

    // A different direction on the same node is a different face, and so is a different level.
    was_new = false;
    store.claim(FaceKey{1, 2, 3, 0, 5}, 3, &was_new);
    CHECK(was_new);
    was_new = false;
    store.claim(FaceKey{1, 2, 3, 3, 4}, 3, &was_new);
    CHECK(was_new);

    // And a full table, where nothing was put anywhere. Four faces, four claimed, no cold ones to
    // take: the answer is kNoFace and it is not new.
    was_new = false;
    REQUIRE(store.claim(FaceKey{9, 9, 9, 0, 0}, 4, &was_new) != kNoFace);
    was_new = true;
    CHECK(store.claim(FaceKey{8, 8, 8, 0, 0}, 4, &was_new) == kNoFace);
    CHECK_FALSE(was_new);
}

// R9b, and it is the clause that makes R9a safe to land at all: the off-screen set is bounded and
// the on-screen set is not bounded by it.
//
// The failure this is written against is D502 with a new cause. The store filling is not a slow
// frame — it is a claim REFUSED, which leaves a visible surface with no face of its own, falling to
// the card's provisional stand-in that re-measures every frame. That is the blocky flickering light
// a player reported. A gathering ray's set has no bound the screen knows about, so without a cap it
// would reach that state on its own, and the fault would arrive looking exactly like a bug that was
// already fixed.
TEST_CASE("the off-screen set is capped, and a pixel's claim never is") {
    FaceStore store;
    FaceStoreBudget budget;
    budget.max_faces = 64;
    budget.cold_frames = 600;
    budget.claim_period = 1;
    budget.secondary_share = 4;   // a quarter of the table, so sixteen faces
    store.create(budget);
    REQUIRE(store.secondary_cap() == 16);

    for (u32 i = 0; i < 16; ++i) {
        REQUIRE(store.claim(FaceKey{i, 7, 0, 0, 0}, 1, nullptr, true) != kNoFace);
    }
    CHECK(store.stats().secondary == 16);

    // The seventeenth is DECLINED: no slot, no refusal counted, and the store is not out of room.
    // Those three together are the whole of what "a class that overruns degrades its own refresh
    // rate and nothing else's" means, and a decline counted as a refusal would read as the fault
    // above in every log line that reports it.
    CHECK(store.claim(FaceKey{17, 7, 0, 0, 0}, 1, nullptr, true) == kNoFace);
    CHECK(store.stats().secondary_declined == 1);
    CHECK(store.stats().refusals == 0);
    CHECK_FALSE(store.out_of_room());

    // ...while the on-screen set still has the other three quarters, and takes them.
    for (u32 i = 0; i < 48; ++i) {
        REQUIRE(store.claim(FaceKey{i, 9, 0, 0, 0}, 1) != kNoFace);
    }
    CHECK(store.stats().faces == 64);
    CHECK(store.validate());
}

// The cap bounds the off-screen set against the table; it does not bound the two sets TOGETHER, and
// the on-screen set has no bound at all. Flying, it has measured 995,684 live faces against a table
// of 1,048,576 (D504) — so a quarter of the table held off screen on top of that is the store full
// and refusing, and a refusal is a visible surface with no light of its own. That is the picture
// D502 was reported as, and R9a must not be able to produce it.
TEST_CASE("a store under pressure stops growing the off-screen set at all") {
    FaceStore store;
    FaceStoreBudget budget;
    budget.max_faces = 1024;
    budget.cold_frames = 600;
    budget.claim_period = 1;
    budget.secondary_share = 4;    // 256 slots, which this never reaches
    budget.pressure_from = 8;      // the squeeze begins under an eighth free
    store.create(budget);

    // The on-screen set takes seven eighths of the table, which is where the squeeze begins.
    for (u32 i = 0; i < 900; ++i) {
        REQUIRE(store.claim(FaceKey{i, 0, 0, 0, 0}, 1) != kNoFace);
    }
    REQUIRE(store.pressure_shift() > 0);
    REQUIRE(store.stats().secondary < store.secondary_cap());

    // A light ray asking now is declined although its own cap has room, because the table has not.
    CHECK(store.claim(FaceKey{1, 1, 1, 0, 0}, 1, nullptr, true) == kNoFace);
    CHECK(store.stats().secondary_declined == 1);
    CHECK(store.stats().refusals == 0);
    // ...and a pixel asking at the same moment is served, which is the whole point of the rule.
    CHECK(store.claim(FaceKey{2, 2, 2, 0, 0}, 1) != kNoFace);
    CHECK(store.validate());
}

// The class is a state and not a birthmark, and this is the case that says so: a face a bounce ray
// found, which the player then walks up to. Without promotion it would count against the off-screen
// cap for the rest of its life — so the cap would be holding down faces somebody is looking at,
// which is the exact opposite of what it is for.
TEST_CASE("a pixel reading an off-screen face moves it into the on-screen set") {
    FaceStore store = make_store(1024, 600);
    bool was_new = false;
    const u32 slot = store.claim(FaceKey{5, 6, 7, 0, 2}, 1, &was_new, true);
    REQUIRE(slot != kNoFace);
    REQUIRE(was_new);
    REQUIRE(store.is_secondary(slot));
    CHECK(store.stats().secondary == 1);

    // The read report, which is what the visibility pass sends for every face a pixel resolves to.
    store.promote(slot);
    CHECK_FALSE(store.is_secondary(slot));
    CHECK(store.stats().secondary == 0);
    CHECK(store.stats().promotions == 1);

    // And a light ray asking for it again does not put it back: the class only ever moves towards
    // the screen. A ray landing on the wall in front of you must not file that wall as off-screen.
    store.claim(FaceKey{5, 6, 7, 0, 2}, 2, nullptr, true);
    CHECK_FALSE(store.is_secondary(slot));
    CHECK(store.stats().secondary == 0);

    // A primary claim of a face that is still secondary promotes it too, which is the same rule
    // arriving through the request lattice rather than through the read report.
    const u32 other = store.claim(FaceKey{8, 8, 8, 0, 1}, 3, nullptr, true);
    REQUIRE(other != kNoFace);
    CHECK(store.stats().secondary == 1);
    store.claim(FaceKey{8, 8, 8, 0, 1}, 4);
    CHECK(store.stats().secondary == 0);
    CHECK(store.validate());
}

// The counter is maintained incrementally and everything else in `validate` is derived, so it is the
// one number here that can drift. A decrement missed on eviction would let the cap creep upward for
// the rest of the run with nothing anywhere to say so — until the store is full of off-screen faces
// and the picture is the one D502 describes.
TEST_CASE("giving up an off-screen face gives its place in the cap back") {
    FaceStore store = make_store(1024, 4);
    for (u32 i = 0; i < 8; ++i) {
        REQUIRE(store.claim(FaceKey{i, 1, 1, 0, 0}, 1, nullptr, true) != kNoFace);
    }
    REQUIRE(store.stats().secondary == 8);

    for (u64 frame = 2; frame < 40; ++frame) store.evict_cold(frame);
    CHECK(store.stats().secondary == 0);
    CHECK(store.find(FaceKey{3, 1, 1, 0, 0}) == kNoFace);
    CHECK(store.validate());
}

TEST_CASE("a reclaimed slot carries nothing of the face before it") {
    FaceStore store = make_store(1024, 4);
    const u32 first = store.claim(FaceKey{3, 3, 3, 3, 0}, 1);
    store.write(first, 0xFFFFFFFFu, 4096, 4096);

    for (u64 frame = 2; frame < 40; ++frame) store.evict_cold(frame);
    REQUIRE(store.find(FaceKey{3, 3, 3, 3, 0}) == kNoFace);

    const u32 second = store.claim(FaceKey{4, 4, 4, 3, 1}, 100);
    REQUIRE(second != kNoFace);
    const GpuFace& face = store.faces()[second];
    CHECK(face.irradiance == 0u);
    CHECK(face_samples(face) == 0u);
    CHECK(face_lit(face) == 0u);
    CHECK(store.validate());
}
