#include <string>

#include "gpu/face_buffers.hpp"

#include <cstring>

#include "core/log.hpp"
// For kLightProbeWords and the dial bits, which are declared once beside the other constants this
// file and the shaders have to agree about.
#include "gpu/render_params.hpp"

namespace ws {

bool FaceBuffers::create(Device& device, const FaceStoreBudget& budget) {
    device_ = &device;

    // The store's records, and then a tail the CPU never writes.
    //
    // The tail is the provisional table (R3e): faces claimed by the card, in the pass that
    // discovers them, so that a surface revealed by a camera move has light in the SAME frame
    // rather than two frames later when the feedback round trip lands. They live in this buffer
    // rather than a second one so that a slot is a slot -- the composite reads
    // `faces.items[slot]` with no tag bit, no branch and no second binding, and `resolve.comp`
    // needed no change at all.
    //
    // Above `max_faces`, so the two allocators can never meet: the CPU hands out [0, watermark)
    // and the card hands out [provisional_base, +kProvisionalFaces). Nothing in the upload path
    // or the audit walks past the watermark, so the card's half is never overwritten by the
    // host's -- which is D295, the fault this arrangement exists to make unrepresentable.
    provisional_base_ = budget.max_faces;
    const u64 face_bytes =
        (static_cast<u64>(budget.max_faces) + kProvisionalFaces) * sizeof(GpuFace);

    // The store's own rule, mirrored here so the shader can be told the capacity as a push
    // constant rather than having to guess it -- and so the two cannot disagree about the mask a
    // bucket index is taken with, which is the sort of divergence that produces a table where
    // half the lookups miss and nothing is obviously wrong.
    entry_capacity_ = 1;
    const u32 wanted = (budget.max_faces * 2 > 1024) ? budget.max_faces * 2 : 1024;
    while (entry_capacity_ < wanted) entry_capacity_ <<= 1;
    const u64 entry_bytes = static_cast<u64>(entry_capacity_) * sizeof(u32);

    // TRANSFER_SRC as well as storage, because the audit reads these back.
    //
    // Without it every mirror check has been an illegal copy: `vkCmdCopyBuffer` from a buffer that
    // was never declared as a copy source is undefined behaviour that happens to work on this
    // driver, and it went unseen because nothing ran with --validation after the audit was
    // written. The check that has found three stale-byte bugs was itself unsound, which is the
    // handover's first trap almost exactly -- ask whether the tool is connected before trusting
    // what it says.
    constexpr VkBufferUsageFlags kStorage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    faces_ = create_device_buffer(device, face_bytes, kStorage, "faces");
    entries_ = create_device_buffer(device, entry_bytes, kStorage, "face entries");

    // The provisional table's buckets, which are its own and not the store's.
    //
    // Sharing the store's bucket array would be the obvious saving and is exactly the bug: that
    // array is uploaded from the host every frame, so anything the card wrote into it is
    // overwritten by a host copy that never knew about it. One word a bucket, and the slot IS the
    // bucket index -- there is no allocator here, which is what lets a claim be a single atomic
    // and lets the pixel that loses the race learn the winner's slot from the value the atomic
    // hands back, in the same instruction, with no ordering between workgroups to depend on.
    const u64 provisional_bytes = static_cast<u64>(kProvisionalFaces) * sizeof(u32);
    provisional_ = create_device_buffer(device, provisional_bytes, kStorage, "face provisional");

    // Big enough for both host arrays at once, which is what the audit reads back and what a first
    // upload of a full store would send. Sized once and never grown: a reallocation mid-play is
    // the hitch streaming exists to avoid. The provisional tail is not staged and not audited, so
    // it is not in here.
    //
    // One region per frame in flight, for the reason node_buffers.cpp sets out in full over its
    // own allocation: the host writes this while RECORDING and the card reads it when it reaches
    // the copy, so a frame sharing a region with one still in flight overwrites its source. Here
    // the corruption lands in face records, where the CPU owns the key -- so a slot ends up naming
    // a different surface than the light in it was measured for, and the picture is a shadow in
    // the wrong place rather than anything that reads as an upload fault.
    //
    // The allocation is split rather than doubled. A frame's claims are bounded by the feedback
    // buffer, which is 131,072 entries, so half of this is still several times the worst frame.
    const u64 staging_bytes =
        static_cast<u64>(budget.max_faces) * sizeof(GpuFace) + entry_bytes;
    staging_capacity_ = staging_bytes / kFramesInFlight;
    staging_ = create_staging_buffer(device, staging_bytes, "face staging");

    stats_ = FaceBufferStats{};
    stats_.device_bytes = face_bytes + entry_bytes + provisional_bytes;
    return faces_.valid() && entries_.valid() && staging_.valid() && provisional_.valid();
}

void FaceBuffers::destroy() {
    if (device_ == nullptr) return;
    destroy_buffer(*device_, faces_);
    destroy_buffer(*device_, entries_);
    destroy_buffer(*device_, provisional_);
    destroy_buffer(*device_, staging_);
    device_ = nullptr;
}

bool FaceBuffers::stage_at(VkCommandBuffer cmd, const void* source, u64 bytes,
                           u64 destination_offset, GpuBuffer& destination) {
    if (bytes == 0) return true;
    if (staging_cursor_ + bytes > staging_capacity_) {
        // Out of staging this frame. Saying so matters: the ranges stay marked, so what did not
        // fit goes next frame rather than being silently skipped and left stale on the card.
        stats_.staging_exhausted = true;
        return false;
    }

    // Inside THIS frame's region: `staging_cursor_` is an offset within the region, not into the
    // buffer. See the note over the allocation.
    const u64 at = staging_frame_base_ + staging_cursor_;
    std::memcpy(static_cast<u8*>(staging_.mapped) + at, source, bytes);

    VkBufferCopy copy{};
    copy.srcOffset = at;
    copy.dstOffset = destination_offset;
    copy.size = bytes;
    vkCmdCopyBuffer(cmd, staging_.buffer, destination.buffer, 1, &copy);

    staging_cursor_ += bytes;
    stats_.uploaded_this_frame += bytes;
    stats_.total_uploaded += bytes;
    return true;
}

// Every dirty face, exactly and only the dirty ones, in one copy with many regions.
//
// Runs are still coalesced where they are genuinely adjacent -- that costs nothing and is not
// what the gap was for.
void FaceBuffers::stage_regions(VkCommandBuffer cmd, FaceStore& store) {
    regions_.clear();
    // runs() hands back a snapshot vector, so marking a run clean inside the loop cannot disturb
    // the iteration.
    for (const auto& run : store.dirty_faces().runs(0)) {
        const u64 bytes = run.second * sizeof(GpuFace);
        if (staging_cursor_ + bytes > staging_capacity_) {
            stats_.staging_exhausted = true;
            break;
        }
        const u64 at = staging_frame_base_ + staging_cursor_;
        std::memcpy(static_cast<u8*>(staging_.mapped) + at,
                    store.faces().data() + run.first, static_cast<usize>(bytes));
        VkBufferCopy copy{};
        copy.srcOffset = at;
        copy.dstOffset = run.first * sizeof(GpuFace);
        copy.size = bytes;
        regions_.push_back(copy);
        staging_cursor_ += bytes;
        stats_.uploaded_this_frame += bytes;
        stats_.total_uploaded += bytes;
        // Staged, so clean. The bytes are in the ring and the copy is recorded on the same command
        // buffer as the pass that reads them; nothing that follows can un-send them.
        if (!whole_set_retry_) store.clear_dirty_faces(run.first, run.second);
    }
    if (!regions_.empty()) {
        vkCmdCopyBuffer(cmd, staging_.buffer, faces_.buffer,
                        static_cast<u32>(regions_.size()), regions_.data());
    }
}

void FaceBuffers::upload(VkCommandBuffer cmd, FaceStore& store, u32 frame_index) {
    stats_.uploaded_this_frame = 0;
    stats_.staging_exhausted = false;
    // The frame's own region of the ring. Writing outside it races the copies of the frame still
    // in flight, and the result is a wrong picture rather than an error.
    staging_frame_base_ = static_cast<u64>(frame_index % kFramesInFlight) * staging_capacity_;
    staging_cursor_ = 0;

    if (store.nothing_dirty()) return;

    // The gap is in elements, and coalescing is worth it because a copy has a fixed cost of its
    // own: sending a few spare records inside one is cheaper than issuing a second.
    const u64 kEntryGap = 256;  // 1 KB of u32

    // No break on failure: a later run may be small enough to fit in what is left. Each run that
    // DID fit is marked clean here, so next frame carries on from where this one stopped rather
    // than restaging the same prefix and running out in the same place.
    for (const auto& run : store.dirty_entries().runs(kEntryGap)) {
        if (stage_at(cmd, store.entries().data() + run.first, run.second * sizeof(u32),
                     run.first * sizeof(u32), entries_)) {
            if (!whole_set_retry_) store.clear_dirty_entries(run.first, run.second);
        }
    }

    // No gap at all on the faces, and this is not a tuning choice.
    //
    // A GpuFace is written from BOTH sides. The CPU owns which face a slot is -- coordinate,
    // level, direction -- and the GPU owns what light is on it. Coalescing sends the clean
    // records that fall inside a run, and the CPU's copy of a clean record has counters of nought
    // because the CPU has never seen the light the card computed. So every upload wiped the sun
    // off up to sixty-three faces around each new one, every frame, for ever.
    //
    // What that looks like from outside is a shadow bug: 11,214 faces pointing at the sun, mean
    // visibility 0.69, and not one of them fully lit -- because a face is bright only until the
    // next claim lands within sixty-four slots of it, which at a few hundred claims a frame is
    // constantly. Nothing about it reads as an upload.
    //
    // The right long-term shape is to split the record so the two owners never share a copy, and
    // that is R3d's business. Until then the rule is that the CPU sends exactly the records it
    // changed, in one call with many regions so the fixed cost coalescing was avoiding is paid
    // once rather than per run.
    stage_regions(cmd, store);

    // Nothing left to clear in the normal arm: every run that was staged was marked clean as it
    // went, and every run that was not is still marked. That is the whole of the backlog fix
    // (D544). The old rule -- clear only when ALL of it fitted -- sounds like the safe one and is
    // not: what it protects against is marking clean what was never sent, and per-run clearing
    // never does that, while the whole-set retry restages the same prefix for ever and leaves the
    // card holding records the store gave up thousands of frames ago.
    if (whole_set_retry_ && !stats_.staging_exhausted) store.clear_dirty();
    if (stats_.staging_exhausted) ++stats_.exhausted_frames;
    ++stats_.uploads;

    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

bool FaceBuffers::audit(const FaceStore& store, VkBuffer seen, u32 frame, u32 seen_window,
                        VkBuffer probe, VkBuffer material) {
    if (device_ == nullptr) return false;

    // Nothing may still be owed to the card, or this compares a host that has moved on against a
    // card that has not and calls the difference a bug.
    //
    // An upload that does not fit its staging ring in one frame leaves its ranges marked and sends
    // them next frame -- deliberately, and the comment above `clear_dirty` says so. The audit had
    // no idea about any of that: it read the buffers whenever it was asked and reported the
    // backlog as a stale byte. Intermittent, scale-dependent, and pointing at the one check that
    // has actually caught three real faults, which is the worst possible thing to make untrustworthy.
    //
    // ...but only the COMPARISON is off. What the card wrote into its own half of the record --
    // which faces have converged, which are still bursting, which class each belongs to -- is a fact
    // about the card and needs no host copy to agree with anything. Skipping the whole audit on a
    // pending upload meant those numbers were never printed in the one case that costs: a MOVING
    // camera always has an upload backlog, and the moving case is where this pass spends 63% of a
    // frame. Two arms of an A/B differed by a factor of two here with nothing in either log to say
    // why, which is trap 15 -- a measurement that never ran looks exactly like a clean one.
    const bool compare = store.nothing_dirty();
    if (!compare) {
        WS_LOG_INFO("faces", "mirror not compared: {} faces and {} buckets still owed to the card",
                    store.dirty_faces().marked(), store.dirty_entries().marked());
    }

    // Wait for the frame BEFORE touching the staging ring, not only after submitting.
    //
    // This reads the device buffers back into the same ring `upload` writes through, and an
    // upload recorded into the frame's command buffer has not necessarily executed yet. Copying
    // over those bytes hands the pending upload whatever the audit just read -- an audit that
    // causes the fault it exists to report, intermittently. The node pool's did exactly that
    // (D265) and it took a per-node eviction pass to make it show up at all.
    device_->wait_idle();

    struct Range {
        const char* name;
        VkBuffer source;
        u64 bytes;
        const void* expected;
        u64 offset;
    };

    // The bucket table only, byte for byte -- and the faces compared on IDENTITY alone below.
    //
    // Ownership is split here and it has to be, because the whole point of the stage is that the
    // card writes the light. The CPU owns which face a slot is: the key, the level, the direction,
    // the flags. The GPU owns what arrives there: irradiance, photons, the sample counters. So a
    // byte-for-byte comparison of the record would report a mismatch on every frame that shaded
    // anything, which is an audit that cries wolf until somebody turns it off -- and this check
    // has already caught three real stale-byte bugs that a photograph never would have.
    Range ranges[1]{
        {"face entries", entries_.buffer, store.entries().size() * sizeof(u32),
         store.entries().data(), 0},
    };
    u64 cursor = 0;
    for (Range& range : ranges) {
        range.offset = cursor;
        cursor += range.bytes;
    }
    // The light probe rides in the same submit, and it rides in THIS one rather than in the second
    // one below, which is the whole point of where it is.
    //
    // The second submit is nested inside "the mirror matched" and "the store's records fit in the
    // staging ring", and neither has anything to do with a counter about what rays found. An
    // instrument that goes quiet when a different instrument is unhappy is D529 exactly: the audit
    // that could not run and the audit with nothing to report were one picture, and the numbers
    // went missing from the only case that costs.
    const u64 probe_offset = cursor;
    const u64 probe_bytes =
        probe != VK_NULL_HANDLE ? static_cast<u64>(kLightProbeWords) * sizeof(u32) : 0;
    cursor += probe_bytes;
    // The WHOLE ring rather than one frame's region of it: this has stalled the device before it
    // got here, so no frame in flight owns any of it, and measuring against the per-frame capacity
    // would silently halve the store this check can cover. Trap 10 in the instrument -- a check
    // that stops looking is indistinguishable from a check that finds nothing.
    if (cursor > staging_.size) {
        WS_LOG_ERROR("faces", "audit needs {} bytes of staging, have {}", cursor, staging_.size);
        return false;
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device_->graphics_family();
    VkCommandPool command_pool = VK_NULL_HANDLE;
    WS_VK(vkCreateCommandPool(device_->handle(), &pool_info, nullptr, &command_pool));

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    WS_VK(vkAllocateCommandBuffers(device_->handle(), &alloc, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    WS_VK(vkBeginCommandBuffer(cmd, &begin));
    for (const Range& range : ranges) {
        if (range.bytes == 0) continue;
        VkBufferCopy copy{};
        copy.srcOffset = 0;
        copy.dstOffset = range.offset;
        copy.size = range.bytes;
        vkCmdCopyBuffer(cmd, range.source, staging_.buffer, 1, &copy);
    }
    if (probe_bytes > 0) {
        VkBufferCopy probe_copy{};
        probe_copy.dstOffset = probe_offset;
        probe_copy.size = probe_bytes;
        vkCmdCopyBuffer(cmd, probe, staging_.buffer, 1, &probe_copy);
    }
    WS_VK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    WS_VK(vkQueueSubmit2(device_->graphics_queue(), 1, &submit, VK_NULL_HANDLE));
    device_->wait_idle();

    // What the gathering ray found this frame, before anything else, because it is the one reading
    // here that depends on nothing else having gone right.
    //
    // Per FRAME, not over the run: the buffer is cleared before every face dispatch, so these are
    // the rays the last frame cast. That is what makes it comparable between two arms -- a lifetime
    // total is dominated by the convergence transient at the start of a run, which is the state
    // `--settle` exists to keep out of every other figure in this file.
    if (probe_bytes > 0) {
        const u32* probe_words =
            reinterpret_cast<const u32*>(static_cast<const u8*>(staging_.mapped) + probe_offset);
        const u32 cast = probe_words[1];
        const u32 sky = probe_words[2];
        const u32 lit = probe_words[3];
        const u32 no_face = probe_words[4];
        const u32 no_light = probe_words[5];
        const u32 coarse = probe_words[6];
        const u32 unbuilt = probe_words[7];
        const u32 off_screen_shaded = probe_words[8];
        if ((probe_words[0] & kProbeOn) == 0) {
            // Said out loud, because nought counters and a switched-off instrument are the same
            // reading and only one of them is a result (trap 15).
            WS_LOG_INFO("faces", "the gathering ray: not counted this run (--no-light-probe)");
        } else {
            const f64 of_cast = cast > 0 ? 100.0 / static_cast<f64>(cast) : 0.0;
            WS_LOG_INFO("faces",
                        "the gathering ray, last frame: {} cast -- {} reached sky ({:.1f}%), {} "
                        "landed on a lit face ({:.1f}%), {} on a surface with no face ({:.1f}%), "
                        "{} on a face with nothing measured yet ({:.1f}%), {} on a cell the pool "
                        "has not built ({:.1f}%)",
                        cast, sky, sky * of_cast, lit, lit * of_cast, no_face, no_face * of_cast,
                        no_light, no_light * of_cast, unbuilt, unbuilt * of_cast);
            // The number R9f is worth, and it is printed whether the rule is on or off -- with it
            // off this is what the change WOULD recover, and with it on it is what it IS
            // recovering. One number, two readings, and the run says which by naming the dial.
            WS_LOG_INFO("faces",
                        "...of the {} that found no light, {} had a coarse face over them that did "
                        "({:.1f}%), and the ray {} reading it",
                        no_face + no_light, coarse,
                        (no_face + no_light) > 0
                            ? 100.0 * coarse / static_cast<f64>(no_face + no_light) : 0.0,
                        (probe_words[0] & kProbeCoarseBounce) != 0 ? "IS" : "is NOT");
            // R9b's budget, counted where it is spent. Read it against the `off-screen stride` on
            // the `faces:` line, which is what the host PREDICTED this would be: the two agreeing is
            // the only thing that says the class is being budgeted rather than merely divided.
            // Nought here with a non-nought stride is the class silently not casting -- which is the
            // shape trap 20 takes in this pass, and the reason a convergence figure is printed beside
            // every timing in it.
            WS_LOG_INFO("faces",
                        "the off-screen set cast on {} faces this frame, out of its own budget",
                        off_screen_shaded);
            // R4c's pool, counted where the blocks are handed out. Three numbers because they are
            // three different states and only one of them is a problem:
            //
            //   HELD is how many faces the pool is serving, and it is what the size of the pool has
            //   to be read against. It is per FRAME and not per store: a face stamps its block on
            //   the frames the shading pass visits it, which is one in `face_stride`, so this is
            //   about a fifth of the population holding blocks;
            //   DECLINED is a face that asked and found every way of its set held by something warm
            //   and worth more. It is not a black surface -- that face reads the hemispherical mean
            //   it already stores -- but it is a reflection that is not the one it should have, and
            //   a large number here is the pool being too small;
            //   TAKEN OVER is the one that separates a full pool from a THRASHING one. Two faces
            //   trading a block every frame both show up as held, both look served, and neither
            //   ever accumulates enough samples to say anything.
            if ((probe_words[0] & kProbeLobe) != 0) {
                const u32 held = probe_words[kProbeLobeHeld];
                const u32 declined = probe_words[kProbeLobeDeclined];
                WS_LOG_INFO("faces",
                            "the lobe pool: {} faces holding a block of {} blocks this frame, "
                            "{} asked and were turned away ({:.1f}%), {} taken over from another "
                            "face",
                            held, kLobeBlocks, declined,
                            (held + declined) > 0
                                ? 100.0 * declined / static_cast<f64>(held + declined) : 0.0,
                            probe_words[kProbeLobeTaken]);
                // R4b's own ray, and the convergence figure that has to be printed beside it.
                //
                // Trap 20 is the standing reason: a lobe that has stopped bursting and a lobe that
                // was never given a ray both cost nothing, and only one of them has an answer. A
                // timing from this pass with `still bursting` at nought means the term is finished;
                // the same timing with it at twenty thousand means the burst has not started.
                WS_LOG_INFO("faces",
                            "the lobe ray: {} cast this frame of {} gathering rays ({:.1f}%), "
                            "{} faces still bursting",
                            probe_words[kProbeLobeRays], cast,
                            cast > 0 ? 100.0 * probe_words[kProbeLobeRays] / static_cast<f64>(cast)
                                     : 0.0,
                            probe_words[kProbeLobeBursting]);
                // R4b's coverage rule, and it needs its own line because "nobody earns the
                // expensive class" and "the pool has no room for it" are different faults with
                // different fixes, and both print as an unchanged picture.
                WS_LOG_INFO("faces",
                            "...of which {} hold the sharp class ({:.1f}%), which is four blocks "
                            "and {} bins against {}",
                            probe_words[kProbeLobeBigHeld],
                            held > 0 ? 100.0 * probe_words[kProbeLobeBigHeld]
                                           / static_cast<f64>(held)
                                     : 0.0,
                            kLobeBigBins, kLobeBins);
            } else {
                // Trap 15 once more: a pool nobody is asking about and a pool with nothing in it
                // print the same nought, and only one of the two is a measurement.
                WS_LOG_INFO("faces",
                            "the lobe pool: not asked this run (--no-face-lobe), so no face has "
                            "outgoing bins and a metal reflects its hemispherical mean");
            }
            if (unbuilt > 0) {
                std::string levels;
                for (u32 level = 0; level < 15; ++level) {
                    const u32 at = probe_words[kLightProbeLevels + level];
                    if (at == 0) continue;
                    if (!levels.empty()) levels += "  ";
                    levels += std::to_string(level) + ":" + std::to_string(at);
                }
                // R10's open item has never had this. A ray stopped by an unbuilt brick has lost
                // 25 cm of sky; one stopped by a shed 512 m subtree has lost a quarter of the
                // county, and a single count cannot tell those apart.
                WS_LOG_INFO("faces", "gathering rays stopped by unbuilt geometry, by level  {}",
                            levels);
            }
        }
    }

    // Whether R4a's arm is on, taken from the dial the card was actually handed rather than from an
    // option the host believes it passed. It is read here because the census that needs it runs in
    // the second submit, and a switched-off instrument reading nought and a store with nothing in it
    // reading nought are the same line otherwise (trap 15).
    const bool materials_on =
        probe_bytes > 0 &&
        (reinterpret_cast<const u32*>(static_cast<const u8*>(staging_.mapped) + probe_offset)[0] &
         kProbeMaterial) != 0;

    bool matched = compare;
    for (const Range& range : ranges) {
        if (!compare) break;
        if (range.bytes == 0) continue;
        const u8* card = static_cast<const u8*>(staging_.mapped) + range.offset;
        const u8* host = static_cast<const u8*>(range.expected);
        if (std::memcmp(card, host, static_cast<usize>(range.bytes)) == 0) continue;
        matched = false;
        // Named to the byte. A mismatch reported as "the picture is wrong somewhere" costs a day;
        // reported as an offset and two values it costs a reading of whatever writes that field.
        for (u64 i = 0; i < range.bytes; ++i) {
            if (card[i] == host[i]) continue;
            WS_LOG_ERROR("faces",
                         "GPU mirror mismatch in {}: first at byte {} of {} (card 0x{:02x}, "
                         "host 0x{:02x})", range.name, i, range.bytes, card[i], host[i]);
            break;
        }
    }
    // And the faces, on identity only. A slot the CPU thinks is face A while the card thinks it is
    // face B is the fault that matters: the light would be written to the wrong surface and look
    // like a shading bug for ever. What is IN the face is the card's business.
    const u32 watermark = store.watermark();
    if ((matched || !compare) && watermark > 0) {
        const u64 face_bytes = static_cast<u64>(watermark) * sizeof(GpuFace);
        if (face_bytes <= staging_.size) {
            VkCommandPoolCreateInfo face_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            face_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            face_pool_info.queueFamilyIndex = device_->graphics_family();
            VkCommandPool face_pool = VK_NULL_HANDLE;
            WS_VK(vkCreateCommandPool(device_->handle(), &face_pool_info, nullptr, &face_pool));
            VkCommandBufferAllocateInfo face_alloc{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            face_alloc.commandPool = face_pool;
            face_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            face_alloc.commandBufferCount = 1;
            VkCommandBuffer face_cmd = VK_NULL_HANDLE;
            WS_VK(vkAllocateCommandBuffers(device_->handle(), &face_alloc, &face_cmd));
            VkCommandBufferBeginInfo face_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            face_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            WS_VK(vkBeginCommandBuffer(face_cmd, &face_begin));
            VkBufferCopy face_copy{};
            face_copy.size = face_bytes;
            vkCmdCopyBuffer(face_cmd, faces_.buffer, staging_.buffer, 1, &face_copy);
            // And the seen stamps behind them, in the same submit, because the two have to be read
            // at the same instant: a face counted as live here and as unseen a moment later is a
            // number that describes neither frame.
            const u64 seen_bytes = static_cast<u64>(watermark) * sizeof(u32);
            const bool seen_fits = seen != VK_NULL_HANDLE &&
                                   face_bytes + seen_bytes <= staging_.size;
            if (seen_fits) {
                VkBufferCopy seen_copy{};
                seen_copy.dstOffset = face_bytes;
                seen_copy.size = seen_bytes;
                vkCmdCopyBuffer(face_cmd, seen, staging_.buffer, 1, &seen_copy);
            }
            // ...and what each of them is made of, in the same submit and for the same reason.
            const u64 material_bytes = static_cast<u64>(watermark) * sizeof(u32);
            const bool material_fits =
                seen_fits && material != VK_NULL_HANDLE &&
                face_bytes + seen_bytes + material_bytes <= staging_.size;
            if (material_fits) {
                VkBufferCopy material_copy{};
                material_copy.dstOffset = face_bytes + seen_bytes;
                material_copy.size = material_bytes;
                vkCmdCopyBuffer(face_cmd, material, staging_.buffer, 1, &material_copy);
            }
            WS_VK(vkEndCommandBuffer(face_cmd));
            VkCommandBufferSubmitInfo face_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            face_info.commandBuffer = face_cmd;
            VkSubmitInfo2 face_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            face_submit.commandBufferInfoCount = 1;
            face_submit.pCommandBufferInfos = &face_info;
            WS_VK(vkQueueSubmit2(device_->graphics_queue(), 1, &face_submit, VK_NULL_HANDLE));
            device_->wait_idle();

            const GpuFace* card = static_cast<const GpuFace*>(staging_.mapped);
            for (u32 slot = 0; compare && slot < watermark; ++slot) {
                const GpuFace& host = store.faces()[slot];
                if (card[slot].x == host.x && card[slot].y == host.y && card[slot].z == host.z &&
                    card[slot].packed == host.packed) {
                    continue;
                }
                matched = false;
                WS_LOG_ERROR("faces",
                             "GPU mirror mismatch: slot {} is ({},{},{}) packed 0x{:08x} on the "
                             "card and ({},{},{}) packed 0x{:08x} here", slot, card[slot].x,
                             card[slot].y, card[slot].z, card[slot].packed, host.x, host.y,
                             host.z, host.packed);
                break;
            }
            // And what the card WROTE into them, which is the one field no check here can
            // compare against a host copy: the CPU claims a face and never touches its light
            // again. Without this the face pass is a stage whose only evidence is the picture,
            // and a picture cannot tell "every face is shadowed" from "every face is unlit" --
            // both are a dark building, and they have nothing in common to fix.
            // Split by whether the face can see the sun at all, because the two populations mean
            // different things and their average means neither. A face pointing away from the sun
            // is meant to read nought and needs no ray; only the ones pointing at it are evidence
            // about whether the shadow ray works.
            const f32 sun[3] = {0.4f, 0.85f, 0.3f};
            const f32 normals[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            u32 settled = 0;
            u32 facing = 0;
            u32 dark = 0;
            u32 bright = 0;
            f32 brightest = 0.0f;
            u32 exhausted = 0;
            u32 by_ignorance = 0;
            u32 ignorance_level[16]{};
            u64 sample_sum = 0;
            f64 facing_sum = 0.0;
            // How much of the store still owes the ambient term rays, which is the one thing the
            // cost of this pass is a function of and the one thing no picture can show: a face that
            // has finished and a face that is one sample short look identical in every view, and
            // only the second is being paid for. Counted over the WHOLE store rather than over the
            // settled faces below it, because the faces that keep this pass busy are overwhelmingly
            // the ones nothing is looking at.
            //
            // Over the LIVE faces, not over the watermark. A slot below the watermark that has been
            // evicted is not a face and has nothing to finish, and counting it as unfinished is
            // trap 10 in the instrument: the first version of this line reported 21,679 faces still
            // casting rays when the true number was three, because 21,675 slots were simply dead.
            u32 ambient_live = 0;
            u32 ambient_done = 0;
            u32 ambient_idle = 0;
            u32 lamp_idle = 0;
            // And how many of them the frame in front of you can actually SEE, which is the
            // number the other three have to be read against. A store six hundred frames deep and
            // a screen are very different populations, and the pass used to shade the first while
            // the picture is made of the second.
            u32 seen_now = 0;
            u32 seen_bursting = 0;
            // R9e. The set is the subject of R9, so the set is what has to be countable: which class
            // each live face belongs to, how far each class has got, and -- the one the plan names
            // as the measurement that decides the stage -- SAMPLES PER FACE PER CLASS. The risk R9
            // carries is not frame time, which the cap fixes by construction; it is a store spread
            // too thin, where every face gets too few samples and the picture gets noisier
            // everywhere rather than slower anywhere. Only this number shows that.
            u32 secondary_live = 0;
            u32 secondary_done = 0;
            u32 secondary_seen = 0;
            // The coarse pyramid, counted apart from both for the reason the loop below sets out.
            u32 stand_in_live = 0;
            u32 stand_in_seen = 0;
            u32 stand_in_done = 0;
            u64 secondary_samples = 0;
            u64 primary_samples = 0;
            u32 primary_live = 0;
            const u32* stamps =
                seen_fits ? reinterpret_cast<const u32*>(static_cast<const u8*>(staging_.mapped) +
                                                         face_bytes)
                          : nullptr;
            for (u32 slot = 0; slot < watermark; ++slot) {
                if (!face_live(card[slot])) continue;
                // The class comes from the STORE and the light comes from the card, which is the
                // record's ownership split read the right way round. See FaceStore::is_secondary.
                if (store.is_secondary(slot)) {
                    // Counted by the set line below and by nothing here, and that is trap 10 rather
                    // than tidiness. An off-screen face casts no rays at all -- it is not idle, it
                    // is not finished, and it is emphatically not "still bursting", which is what
                    // `live - idle` called it: the first run of R9a reported 284,662 faces bursting
                    // when the true number was 22,517 and the other 262,144 were costing nothing.
                    // A counter that gives one number to two states sends the next reader to the
                    // wrong pass, and this line is read before every cost figure in this file.
                    ++secondary_live;
                    if ((card[slot].photons & kFaceAmbientDone) != 0) ++secondary_done;
                    secondary_samples += face_samples(card[slot]);
                    if (stamps != nullptr && (frame - stamps[slot]) <= seen_window) ++secondary_seen;
                    continue;
                }
                // ...and the coarse pyramid is the same distinction one class along (R9f). A
                // stand-in is a PRIMARY face -- a pixel asked for the fine face under it -- and
                // while no pixel is reading it, `may_cast` is false and it casts nothing at all. So
                // counting it here read as 21,783 faces "still bursting" that were costing not one
                // ray, which is the sentence the secondary block above is written for, arriving one
                // class along. A counter that gives one number to two states sends the next reader
                // to the wrong pass, and this line is read before every cost figure in this file.
                if (store.is_stand_in(slot)) {
                    ++stand_in_live;
                    if (stamps != nullptr && (frame - stamps[slot]) <= seen_window) ++stand_in_seen;
                    if ((card[slot].photons & kFaceAmbientDone) != 0) ++stand_in_done;
                    continue;
                }
                ++primary_live;
                primary_samples += face_samples(card[slot]);
                ++ambient_live;
                const bool done = (card[slot].photons & kFaceAmbientDone) != 0;
                if (done) ++ambient_done;
                if ((card[slot].photons & kFaceAmbientIdle) != 0) ++ambient_idle;
                if ((card[slot].photons & kFaceLampIdle) != 0) ++lamp_idle;
                if (stamps != nullptr && (frame - stamps[slot]) <= seen_window) {
                    ++seen_now;
                    if (!done) ++seen_bursting;
                }
            }
            WS_LOG_INFO("faces",
                        "ambient on the card: {} of {} live faces cast no more rays at all, {} are "
                        "quiet except on the sun's own frames, {} still bursting",
                        ambient_done, ambient_live, ambient_idle - ambient_done,
                        ambient_live - ambient_idle);
            // And the same question for the lamps, which is a different one and has to be asked
            // separately: the two terms converge at different rates, are reopened by different
            // announcements, and a face can be silent about the sky while still bursting at a
            // sconce. Reporting one number for both would be trap 7 in the instrument -- "nothing
            // to do" and "nothing to do about THIS" are not one answer.
            //
            // In a scene with no emitters in it every live face reads finished on the first frame
            // it is shaded, and that is the number to check before believing any cost figure here:
            // the facility has no lamps, so this pass owes the lamps nothing there and a change in
            // its timing on that scene is not about this term.
            WS_LOG_INFO("faces", "lamps on the card: {} of {} live faces cast no more rays at all",
                        lamp_idle, ambient_live);
            // The coarse pyramid on the card. `seen` is the number that matters: a stand-in a pixel
            // is reading is standing in for fine faces that are missing, which is the state R9d and
            // R9f both exist for, and one nobody is reading is holding an answer for when somebody
            // comes back. Neither is a face that owes rays, which is why they are out of the line
            // above.
            WS_LOG_INFO("faces",
                        "the coarse pyramid on the card: {} stand-ins, {} of them read by a pixel "
                        "in the last {} frames, {} have finished their ambient term",
                        stand_in_live, stand_in_seen, seen_window + 1, stand_in_done);
            // The one that says whether the pass is working on the picture or on the store's
            // memory of it. Everything outside `seen` is costing nothing now and is why: light
            // stops at the edge of what a pixel read, and residency does not.
            if (stamps != nullptr) {
                WS_LOG_INFO("faces",
                            "seen on the card: {} of {} live faces were read by a pixel in the "
                            "last {} frames; {} of those still owe ambient rays",
                            seen_now, ambient_live, seen_window + 1, seen_bursting);
            }
            // How far behind the store the card actually is, which is the number every cost figure
            // in this pass has to be read against and which nothing printed.
            //
            // The shading pass shades what the CARD holds. The host evicting a face marks the slot
            // dirty, and an upload that runs out of staging sends what fits and leaves the rest
            // marked -- so the gap is at most the backlog still owed, and it shrinks by a staging
            // region a frame. Before D544 an exhausted frame cleared NOTHING and restaged the same
            // prefix, so the gap did not shrink at all: measured flying at 1440p, 982,507 live on
            // the card against 548,601 in the store, and every one of those is a face the worklist
            // may still pack and the shading pass may still pay for.
            const i64 behind = static_cast<i64>(ambient_live) + static_cast<i64>(secondary_live) +
                               static_cast<i64>(stand_in_live) -
                               static_cast<i64>(store.stats().faces);
            WS_LOG_INFO("faces",
                        "the card is {} records ahead of the store ({} live there, {} here); the "
                        "upload ran out of staging on {} frames, and a frame that runs out sends "
                        "what fits and carries the rest over",
                        behind, ambient_live + secondary_live + stand_in_live,
                        store.stats().faces, stats_.exhausted_frames);
            // R9e, and it is printed whether or not the off-screen set has anything in it: nought
            // secondary faces on a run with the rule ON is itself the result, and a line that only
            // appears when something happened cannot say that nothing did.
            WS_LOG_INFO("faces",
                        "the set on the card: {} on screen at {:.0f} sun samples each, {} off "
                        "screen at {:.0f} -- {} of those have finished their ambient term and {} "
                        "are being read by a pixel and should not be in that class",
                        primary_live, primary_live > 0
                                          ? static_cast<f64>(primary_samples) / primary_live : 0.0,
                        secondary_live,
                        secondary_live > 0
                            ? static_cast<f64>(secondary_samples) / secondary_live : 0.0,
                        secondary_done, secondary_seen);
            for (u32 slot = 0; slot < watermark; ++slot) {
                if (face_samples(card[slot]) < kFaceEager) continue;
                ++settled;
                const u32 direction = face_direction(card[slot]);
                if (direction >= 6) continue;
                const f32 towards = normals[direction][0] * sun[0] +
                                    normals[direction][1] * sun[1] + normals[direction][2] * sun[2];
                if (towards <= 0.0f) continue;
                ++facing;
                const f32 visibility = face_visibility(card[slot]);
                facing_sum += visibility;
                if (visibility < 0.03f) ++dark;
                if (visibility > 0.97f) ++bright;
                if (visibility > brightest) brightest = visibility;
                sample_sum += face_samples(card[slot]);
                // The STEP COUNT is bits 0-23 and nothing else. The mask used to be 0x7FFFFFFF,
                // which is every bit but the ignorance flag -- so the level in bits 24-27 read as
                // tens of millions of steps and any face that had ever been stopped by ignorance
                // counted as exhausted. It did not show while those were the only other bits used;
                // it would have the moment anything else was packed beside them, which is what
                // kFaceAmbientDone now does.
                if (visibility > 0.03f && (card[slot].photons & 0x00FFFFFFu) >= 500) ++exhausted;
                if (visibility < 0.03f && (card[slot].photons & 0x80000000u) != 0) {
                    ++by_ignorance;
                    ++ignorance_level[(card[slot].photons >> 24) & 0xFu];
                }
            }
            if (settled > 0) {
                WS_LOG_INFO("faces",
                            "sun on the card: {} of {} settled, {} face the sun -- of those, mean "
                            // Four decimals because the gate this instrument exists to check is
                            // "stays at 0.0000": at two, a mean of 0.0049 -- one face in a
                            // thousand wrongly in full sun -- prints as 0.00 and reads as passing.
                            "visibility {:.4f}, brightest {:.4f}, {} fully shadowed, {} fully lit, "
                            "{:.0f} samples each, {} lit only by a ray that ran out of steps, "
                            "{} shadowed by a cell the pool has not built",
                            // Against the ON-SCREEN population, not against the watermark. An
                            // off-screen face has no sun samples by construction and never will
                            // until a pixel reads it, so counting it in the denominator reads as a
                            // third of the store having failed to settle.
                            settled, primary_live, facing,
                            facing > 0 ? facing_sum / facing : 0.0, brightest, dark, bright,
                            facing > 0 ? static_cast<f64>(sample_sum) / facing : 0.0, exhausted,
                            by_ignorance);
                if (by_ignorance > 0) {
                    std::string levels;
                    for (u32 level = 0; level < 16; ++level) {
                        if (ignorance_level[level] == 0) continue;
                        if (!levels.empty()) levels += "  ";
                        levels += std::to_string(level) + ":" +
                                  std::to_string(ignorance_level[level]);
                    }
                    WS_LOG_INFO("faces", "shadowed by ignorance, by level  {}", levels);
                }
            }

            // ---- what the store is MADE of (R4a) -------------------------------------------
            //
            // The number that sizes every part of R4, and the one a picture cannot give. A
            // directional payload is allocated only where it is read, so what has to be known
            // before any of it is built is how much of a real store is specular at all -- and
            // whether the faces a PIXEL is looking at are a different population from the store as
            // a whole, which they will be, because gilt and bronze are on the parts of a building
            // that face you.
            //
            // Reported as a distribution and not as a count past a threshold, deliberately. There
            // is no roughness threshold anywhere in this stage and there is not going to be one:
            // bins come from roughness and coverage as continuous quantities, and an instrument
            // that picks a cutoff is how a cutoff ends up in the shader six weeks later because
            // "that is the number we have always reported".
            if (!materials_on) {
                WS_LOG_INFO("faces",
                            "what the faces are made of: not asked this run "
                            "(--no-face-materials), so nothing here knows");
            } else if (material_fits) {
                const u32* made_of = reinterpret_cast<const u32*>(
                    static_cast<const u8*>(staging_.mapped) + face_bytes + seen_bytes);
                constexpr u32 kKnown = 1u << 31;
                constexpr u32 kNone = 1u << 30;
                u32 known = 0;
                u32 waiting = 0;
                u32 coarse = 0;
                u32 metal = 0;
                u32 seen_known = 0;
                u32 seen_metal = 0;
                u64 rough_sum = 0;
                // Quarters of the roughness range, which is the honest way to show a quantity that
                // is going to be read continuously: [0,64) is a mirror, [192,255] is matte stone.
                u32 quarters[4]{};
                u32 seen_quarters[4]{};
                for (u32 slot = 0; slot < watermark; ++slot) {
                    if (!face_live(card[slot])) continue;
                    const u32 word = made_of[slot];
                    const bool on_screen =
                        stamps != nullptr && (frame - stamps[slot]) <= seen_window;
                    // A coarse face has looked and has no material to have, and it is counted apart
                    // from both of the others: as "waiting" it would read as the pool being behind,
                    // and as "known" it would need a roughness, which it has not got.
                    if ((word & kNone) != 0) {
                        ++coarse;
                        continue;
                    }
                    if ((word & kKnown) == 0) {
                        ++waiting;
                        continue;
                    }
                    ++known;
                    const u32 rough = word & 0xFFu;
                    rough_sum += rough;
                    ++quarters[rough >> 6];
                    if (((word >> 8) & 0xFFu) > 0) ++metal;
                    if (on_screen) {
                        ++seen_known;
                        ++seen_quarters[rough >> 6];
                        if (((word >> 8) & 0xFFu) > 0) ++seen_metal;
                    }
                }
                WS_LOG_INFO("faces",
                            "what the faces are made of: {} carry a material, {} are coarse and "
                            "cannot, {} have not looked yet; of those that carry one, roughness "
                            "quarters {} / {} / {} / {} (mean {:.0f}) and {} carry some metal",
                            known, coarse, waiting, quarters[0], quarters[1], quarters[2],
                            quarters[3], known > 0 ? static_cast<f64>(rough_sum) / known : 0.0,
                            metal);
                WS_LOG_INFO("faces",
                            "...and of the {} a pixel is reading, roughness quarters {} / {} / {} / "
                            "{} and {} carry some metal",
                            seen_known, seen_quarters[0], seen_quarters[1], seen_quarters[2],
                            seen_quarters[3], seen_metal);
            } else if (material != VK_NULL_HANDLE) {
                // Said out loud rather than left as an absent line, because a census that could not
                // run and a store with nothing specular in it are the same silence (trap 15).
                WS_LOG_INFO("faces", "what the faces are made of: not read back (staging is full)");
            }

            // ...and the card's OWN faces, which are the most expensive face in the store and which
            // nothing has ever counted.
            //
            // A provisional stand-in is claimed by the visibility pass when a pixel's fine face is
            // not answerable and the coarse face over it is not in the store either (R3e, D316). Its
            // record is rewritten every frame it is needed, so it can never accumulate: it takes a
            // fresh unbounded far ray and a fresh lamp burst EVERY FRAME, for as long as anything is
            // looking at it. Half a million of the store's faces cost a few rays each and then stop;
            // these cost their rays for ever, and there was no number anywhere saying how many there
            // were. Trap 16 -- when a cost is suspect, count the events that produce it.
            //
            // The mark carries the low byte of the frame it was claimed in, so "claimed on the frame
            // that has just been drawn" is one comparison. The two frames before it are counted too,
            // because a stand-in claimed on the frame before this one is a stand-in that was being
            // paid for a moment ago, and a single frame's count on a moving camera is a sample of a
            // quantity that varies frame to frame.
            const u64 marks_bytes = static_cast<u64>(provisional_count()) * sizeof(u32);
            if (marks_bytes <= staging_.size) {
                VkCommandPoolCreateInfo mark_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                mark_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                mark_pool_info.queueFamilyIndex = device_->graphics_family();
                VkCommandPool mark_pool = VK_NULL_HANDLE;
                WS_VK(vkCreateCommandPool(device_->handle(), &mark_pool_info, nullptr, &mark_pool));
                VkCommandBufferAllocateInfo mark_alloc{
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                mark_alloc.commandPool = mark_pool;
                mark_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                mark_alloc.commandBufferCount = 1;
                VkCommandBuffer mark_cmd = VK_NULL_HANDLE;
                WS_VK(vkAllocateCommandBuffers(device_->handle(), &mark_alloc, &mark_cmd));
                VkCommandBufferBeginInfo mark_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                mark_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                WS_VK(vkBeginCommandBuffer(mark_cmd, &mark_begin));
                VkBufferCopy mark_copy{};
                mark_copy.size = marks_bytes;
                vkCmdCopyBuffer(mark_cmd, provisional_.buffer, staging_.buffer, 1, &mark_copy);
                WS_VK(vkEndCommandBuffer(mark_cmd));
                VkCommandBufferSubmitInfo mark_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                mark_info.commandBuffer = mark_cmd;
                VkSubmitInfo2 mark_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
                mark_submit.commandBufferInfoCount = 1;
                mark_submit.pCommandBufferInfos = &mark_info;
                WS_VK(vkQueueSubmit2(device_->graphics_queue(), 1, &mark_submit, VK_NULL_HANDLE));
                device_->wait_idle();

                const u32* marks = static_cast<const u32*>(staging_.mapped);
                u32 fresh = 0;
                u32 recent = 0;
                u32 ever = 0;
                for (u32 i = 0; i < provisional_count(); ++i) {
                    if (marks[i] == 0) continue;
                    ++ever;
                    const u32 stamp = marks[i] >> 24;
                    const u32 age = (frame - stamp) & 0xFFu;
                    if (age == 0) ++fresh;
                    if (age <= 2) ++recent;
                }
                WS_LOG_INFO("faces",
                            "the card's own stand-ins: {} claimed on this frame, {} in the last "
                            "three, {} buckets ever used of {} -- each of these takes a fresh "
                            "unbounded ray and a fresh lamp burst every frame it is needed",
                            fresh, recent, ever, provisional_count());
                vkDestroyCommandPool(device_->handle(), mark_pool, nullptr);
            }

            vkDestroyCommandPool(device_->handle(), face_pool, nullptr);
        }
    }

    if (matched && compare) {
        WS_LOG_INFO("faces", "GPU mirror matches: {} faces by identity, {} buckets exactly",
                    watermark, store.entries().size());
    }

    vkDestroyCommandPool(device_->handle(), command_pool, nullptr);
    // A run that could not compare has not failed; it has not asked. Saying otherwise would make
    // "the mirror is wrong" and "the mirror was busy" one answer, which is the distinction the log
    // line above exists to draw.
    return compare ? matched : true;
}

}  // namespace ws
