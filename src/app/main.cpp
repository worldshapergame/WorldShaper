// WorldShaper - entry point.
//
// Window, device, world, streaming, and the two render passes: a hierarchical ray march
// that writes a visibility buffer, and a resolve that turns it into pixels. Plus two
// headless audit modes that run in CI, because most of what can go wrong here is not
// visible on screen.

#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/arena.hpp"
#include "core/hash.hpp"
#include "core/jobs.hpp"
#include "core/log.hpp"
#include "app/updater.hpp"
#include "core/time.hpp"
#include "core/version.hpp"
#include "debug/hud.hpp"
#include "game/camera.hpp"
#include "game/chisel.hpp"
#include "game/clipboard.hpp"
#include "game/repeat.hpp"
#include "game/toolbelt.hpp"
#include "gpu/device.hpp"
#include "gpu/feedback.hpp"
#include "gpu/image.hpp"
#include "gpu/render_params.hpp"
#include "gpu/profiler.hpp"
#include "gpu/screenshot.hpp"
#include "gpu/shader.hpp"
#include "gpu/swapchain.hpp"
#include "gpu/world_buffers.hpp"
#include "platform/window.hpp"
#include "world/history.hpp"
#include "world/op.hpp"
#include "world/raycast.hpp"
#include "world/residency.hpp"
#include "world/serialize.hpp"
#include "world/test_scene.hpp"
#include "world/voxel_type.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

// Room on the GPU for the ghosts. A *preview* budget, not a limit on the tool: a clip too
// big to fit here is still selected, transformed and stamped exactly — it is drawn as an
// outline instead of as voxels. Thirty-two megabytes each side of the copy.
inline constexpr u64 kMaxClipPoolCells = 8ull * 1024ull * 1024ull;

struct Options {
    bool headless = false;
    bool stream_audit = false;
    u64 stream_frames = 0;
    u64 ticks = 0;
    u32 width = 1600;
    u32 height = 900;
    bool size_explicit = false;   // an explicit size overrides the fit-to-desktop clamp
    bool vsync = true;
    bool validation = (WS_DEBUG != 0);
    bool help = false;
    // A development build should not go looking for a release older than what is sitting in
    // front of you, and a scripted screenshot should not depend on the network.
    bool no_update_check = (WS_DEBUG != 0);
    bool stream_log = false;   // per-second residency report, for diagnosing streaming

    // Render a fixed number of frames, save the last one, and exit. This is how a
    // rendering change gets checked without a person having to look at the screen.
    std::string screenshot;
    u64 screenshot_frame = 30;
    u32 debug_mode = 0;   // 0 shaded, 1 step count, 2 face normals

    // "x,y,z,yaw,pitch" in metres and degrees. Lets a measurement be repeated exactly,
    // which is what makes frame times comparable between builds.
    std::string camera;

    // Scripted chisel, for checking the tool without a person holding the mouse.
    // --edit "x0,y0,z0,x1,y1,z1,material" applies one edit through the history at startup;
    // material 0 carves. --preview takes the same six numbers plus a state (1 carve,
    // 2 place, 3 refused) and forces the preview box on. Both are in voxels.
    std::string edit;
    std::string preview;
    u32 material = 0;   // which entry of the chisel's palette starts selected

    // Scripted clipboard: --clip "x0,y0,z0,x1,y1,z1,dx,dy,dz,copies,turn" captures that box
    // at frame 100, offsets the ghost by (dx,dy,dz), fans out `copies`, and turns it `turn`
    // degrees about y. Everything after the box is optional.
    std::string clip;
};

// Reads up to `count` comma-separated numbers. Missing ones keep their defaults, so a
// short string is a partial override rather than an error.
void parse_numbers(const std::string& text, i64* out, u32 count) {
    const char* cursor = text.c_str();
    for (u32 i = 0; i < count && *cursor != '\0'; ++i) {
        out[i] = std::strtoll(cursor, const_cast<char**>(&cursor), 10);
        if (*cursor == ',') ++cursor;
    }
}

// Frame parameters live in a uniform buffer rather than push constants; see
// gpu/render_params.hpp for why.

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_number = [&](u64 fallback) -> u64 {
            if (i + 1 < argc) return std::strtoull(argv[++i], nullptr, 10);
            return fallback;
        };

        if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--ticks") {
            options.ticks = next_number(0);
            options.headless = true;
        } else if (arg == "--stream-frames") {
            options.stream_frames = next_number(600);
            options.stream_audit = true;
            options.headless = true;
        } else if (arg == "--width") {
            options.width = static_cast<u32>(next_number(options.width));
            options.size_explicit = true;
        } else if (arg == "--height") {
            options.height = static_cast<u32>(next_number(options.height));
            options.size_explicit = true;
        } else if (arg == "--cam") {
            if (i + 1 < argc) options.camera = argv[++i];
        } else if (arg == "--edit") {
            if (i + 1 < argc) options.edit = argv[++i];
        } else if (arg == "--preview") {
            if (i + 1 < argc) options.preview = argv[++i];
        } else if (arg == "--clip") {
            if (i + 1 < argc) options.clip = argv[++i];
        } else if (arg == "--material") {
            options.material = static_cast<u32>(next_number(0));
        } else if (arg == "--debug-mode") {
            options.debug_mode = static_cast<u32>(next_number(0));
        } else if (arg == "--screenshot") {
            if (i + 1 < argc) options.screenshot = argv[++i];
        } else if (arg == "--screenshot-frame") {
            options.screenshot_frame = next_number(30);
        } else if (arg == "--no-vsync") {
            options.vsync = false;
        } else if (arg == "--validation") {
            options.validation = true;
        } else if (arg == "--stream-log") {
            options.stream_log = true;
        } else if (arg == "--no-update-check") {
            options.no_update_check = true;
        } else if (arg == "--version") {
            std::printf("WorldShaper %s\n", kVersion);
            options.help = true;
        } else if (arg == "--no-validation") {
            options.validation = false;
        } else if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else {
            WS_LOG_WARN("app", "unknown argument '{}'", arg);
        }
    }
    return options;
}

void print_help() {
    std::puts(
        "WorldShaper\n"
        "  --headless            run with no window or GPU\n"
        "  --ticks N             headless world audit over N ops, then exit\n"
        "  --stream-frames N     headless streaming audit over N frames, then exit\n"
        "  --width N --height N  window size (default 1600x900)\n"
        "  --no-vsync            uncapped presentation\n"
        "  --validation          force Vulkan validation layers on\n"
        "  --no-validation       force them off\n"
        "  --cam x,y,z,yaw,pitch scripted camera (metres, degrees)\n"
        "  --screenshot FILE     save frame --screenshot-frame N and exit\n"
        "  --debug-mode N        0 shaded, 1 step count, 2 normals, 3 detail, 4 clip ghost\n"
        "  --clip x0,..,z1,dx,dy,dz,copies,turn   scripted clipboard ghost\n"
        "  --edit x0,..,z1,mat   apply one chisel edit at startup (mat 0 carves)\n"
        "  --preview x0,..,z1,s  force the preview box on (s: 1 carve, 2 place, 3 refused)\n"
        "\n"
        "In game:  F1 developer panel   F2 overlay   F5 reload shaders\n"
        "          F11 toggle vsync     Esc quit\n"
        "  chisel: hold LMB carve   RMB place   G+wheel distance   MMB constraint\n"
        "          Z undo   X redo   R clear points   C cancel   Q/E material");
}

// Headless is a first-class mode from Stage 0 (documentation/02, "Headless mode").
// Conservation-of-matter audits, determinism checks and multiplayer desync tests all run
// through this path in CI, so it must never be allowed to rot.
//
// This is the Stage 1 exit check: apply a large random op stream to a real world, assert
// every invariant, audit the matter ledger against a full recount, round-trip the save,
// and report the memory numbers documentation/03 section 8 depends on.
int run_headless(const Options& options) {
    const u64 op_count = (options.ticks > 0) ? options.ticks : 100000;
    WS_LOG_INFO("app", "headless world audit: {} ops", op_count);

    JobSystem jobs;
    TagRegistry tags;
    PropertyRegistry properties;
    VoxelTypeTable types;
    World world;
    MatterLedger ledger;
    OpLog log;

    // A handful of materials sharing one behaviour but differing in colour, which is the
    // shape real terrain has and the one the interning design is tuned for.
    BehaviourRecord stone{};
    stone.material = 1;
    stone.tags.add(tags.find("stone"));
    stone.tags.add(tags.find("solid"));
    stone.properties.set(props::kDensity, PropertyValue::from_uint(2600));

    std::vector<VoxelTypeId> palette;
    for (u32 i = 0; i < 8; ++i) {
        VisualRecord visual{};
        visual.red = static_cast<u8>(90 + i * 8);
        visual.green = static_cast<u8>(88 + i * 7);
        visual.blue = static_cast<u8>(80 + i * 6);
        visual.opacity = 255;
        palette.push_back(types.intern(visual, stone));
    }

    const u64 build_start = now_ns();
    u64 voxels_written = 0;
    for (u64 step = 0; step < op_count; ++step) {
        const u64 h = hash_cell(static_cast<i64>(step), 17, 23, step, 0x57534831ull);
        const i64 x = static_cast<i64>(hash_range(h, 512)) - 256;
        const i64 y = static_cast<i64>(hash_range(hash_mix(h + 1), 256)) - 128;
        const i64 z = static_cast<i64>(hash_range(hash_mix(h + 2), 512)) - 256;
        const i64 size = static_cast<i64>(hash_range(hash_mix(h + 3), 4));
        const u32 roll = hash_range(hash_mix(h + 4), 100);
        const VoxelTypeId type =
            (roll < 25) ? kAir : palette[hash_range(hash_mix(h + 5), 8)];

        const Op op = Op::fill_box(step, static_cast<u32>(step % 4), x, y, z, x + size,
                                   y + size, z + size, type,
                                   type == kAir ? MatterReason::PlayerBreak
                                                : MatterReason::PlayerPlace);
        log.append(op);
        voxels_written += apply_op(world, op, ledger).voxels_changed;
    }
    const f64 build_ms = ns_to_ms(now_ns() - build_start);

    world.compact();

    bool ok = true;
    if (!world.validate()) {
        WS_LOG_ERROR("audit", "world invariants FAILED");
        ok = false;
    }
    if (!ledger.audit(world)) {
        WS_LOG_ERROR("audit", "matter ledger disagrees with a full recount: {} types differ",
                     ledger.audit_failures(world).size());
        ok = false;
    }

    // Save, load, save. The two byte streams must be identical.
    WorldSave source{&tags, &properties, &types, &world};
    ByteWriter first;
    write_save(first, source);

    TagRegistry loaded_tags;
    PropertyRegistry loaded_properties;
    VoxelTypeTable loaded_types;
    World loaded_world;
    WorldSave target{&loaded_tags, &loaded_properties, &loaded_types, &loaded_world};

    ByteReader reader(first.data());
    if (!read_save(reader, target)) {
        WS_LOG_ERROR("audit", "reloading the save FAILED");
        ok = false;
    }

    ByteWriter second;
    write_save(second, target);
    if (first.data() != second.data()) {
        WS_LOG_ERROR("audit", "save -> load -> save is NOT byte-identical ({} vs {} bytes)",
                     first.size(), second.size());
        ok = false;
    }
    if (loaded_world.content_hash() != world.content_hash()) {
        WS_LOG_ERROR("audit", "the reloaded world has a different content hash");
        ok = false;
    }

    const WorldStats stats = world.stats();
    const VoxelTypeStats type_stats = types.stats();

    WS_LOG_INFO("audit", "ops {}  voxels written {}  ({:.1f} M writes/s)", op_count,
                voxels_written,
                (build_ms > 0.0) ? static_cast<f64>(voxels_written) / build_ms / 1000.0 : 0.0);
    WS_LOG_INFO("audit", "chunks {}  bricks {}  solid voxels {}", stats.chunks, stats.bricks,
                stats.solid_voxels);
    // Note on the numbers: this stress world scatters eight materials at random, which is
    // the worst case for palette compression. Coherent terrain, which is what a real world
    // looks like, sits near the 0.4 bytes/voxel documentation/03 Â§3 budgets for.
    WS_LOG_INFO("audit", "world memory {} KB  ({:.3f} bytes/voxel, {:.2f} per solid voxel)",
                stats.bytes / 1024, stats.bytes_per_voxel(), stats.bytes_per_solid_voxel());
    WS_LOG_INFO("audit", "voxel types {}  visuals {}  behaviours {}  dedup {:.4f}",
                type_stats.types, type_stats.visual_records, type_stats.behaviour_records,
                type_stats.dedup_rate());
    WS_LOG_INFO("audit", "save {} KB  world hash {:#018x}  op log hash {:#018x}",
                first.size() / 1024, world.content_hash(), log.rolling_hash());
    WS_LOG_INFO("audit", "job workers {}", jobs.worker_count());

    if (!ok) {
        WS_LOG_FATAL("audit", "FAILED");
        return 1;
    }
    WS_LOG_INFO("audit", "all invariants hold");
    return 0;
}

// The Stage 2 exit check, with no GPU involved: build the scripted test scene, fly a
// camera path over it, and assert after every frame that what the streamer holds is
// bit-identical to what the world holds. Eviction, re-upload and pool churn are all
// exercised because the budget is deliberately too small to hold the scene at once.
int run_stream_audit(const Options& options) {
    const u64 frames = (options.stream_frames > 0) ? options.stream_frames : 600;
    WS_LOG_INFO("app", "streaming audit: {} frames over the test scene", frames);

    TagRegistry tags;
    PropertyRegistry properties;
    VoxelTypeTable types;
    World world;
    MatterLedger ledger;

    const TestScenePalette palette = create_test_palette(types, tags);
    const u64 build_start = now_ns();
    build_test_scene(world, palette, 1024, ledger);
    world.compact();
    const f64 build_ms = ns_to_ms(now_ns() - build_start);

    const WorldStats world_stats = world.stats();
    WS_LOG_INFO("audit", "scene: {} chunks, {} bricks, {} solid voxels, {} MB ({:.3f} B/voxel), built in {:.0f} ms",
                world_stats.chunks, world_stats.bricks, world_stats.solid_voxels,
                world_stats.bytes / (1024 * 1024), world_stats.bytes_per_voxel(), build_ms);

    // Deliberately smaller than the scene so eviction and re-upload are exercised, but
    // large enough to hold the working set the camera path actually looks at â€” a budget
    // that cannot hold what is on screen measures thrashing rather than streaming.
    ResidencyBudget budget;
    budget.payload_bytes = 24ull << 20;
    budget.max_bricks = 80000;
    ResidencyManager residency;
    residency.create(budget, types);

    bool ok = true;
    u64 checked = 0;
    f64 worst_update_ms = 0.0;
    u64 worst_update_frame = 0;
    f64 total_update_ms = 0.0;

    for (u64 frame = 1; frame <= frames; ++frame) {
        const f64 seconds = static_cast<f64>(frame) / 60.0;
        const f64 angle = seconds * 0.25;
        const i64 focus_x = static_cast<i64>(std::cos(angle) * 700.0);
        const i64 focus_z = static_cast<i64>(std::sin(angle) * 700.0);
        const ChunkCoord centre = chunk_coord_of(focus_x, 0, focus_z);

        for (i64 z = -3; z <= 3; ++z) {
            for (i64 y = -1; y <= 1; ++y) {
                for (i64 x = -3; x <= 3; ++x) {
                    const ChunkCoord coord{centre.x + x, centre.y + y, centre.z + z};
                    if (world.has_chunk(coord)) residency.request(coord);
                }
            }
        }

        const u64 update_start = now_ns();
        residency.update(world, frame);
        const f64 update_ms = ns_to_ms(now_ns() - update_start);
        total_update_ms += update_ms;
        if (update_ms > worst_update_ms) {
            worst_update_ms = update_ms;
            worst_update_frame = frame;
        }

        if (!residency.validate()) {
            WS_LOG_ERROR("audit", "residency invariants FAILED at frame {}", frame);
            ok = false;
            break;
        }

        // Sample the shader's own lookup path â€” wrapped grid, record, mask, rank â€” near
        // the camera. This is what catches grid aliasing and stale records, which a
        // per-chunk hash comparison cannot see because it never goes through the grid.
        for (u32 sample = 0; sample < 256; ++sample) {
            const u64 h = hash_cell(static_cast<i64>(frame), sample, 0, frame, 0x57414C4Bull);
            const i64 x = focus_x + static_cast<i64>(hash_range(h, 512)) - 256;
            const i64 y = static_cast<i64>(hash_range(hash_mix(h + 1), 64)) - 32;
            const i64 z = focus_z + static_cast<i64>(hash_range(hash_mix(h + 2), 512)) - 256;
            if (!residency.resident(chunk_coord_of(x, y, z))) continue;
            ++checked;
            if (residency.mirror_voxel_world(x, y, z) != world.get(x, y, z)) {
                WS_LOG_ERROR("audit", "shader-path lookup disagreed at ({}, {}, {}) on frame {}",
                             x, y, z, frame);
                ok = false;
                break;
            }
        }
        if (!ok) break;

        // Everything resident must agree with the world, every frame.
        for (const ChunkCoord& coord : world.sorted_chunk_coords()) {
            if (!residency.resident(coord)) continue;
            ++checked;
            if (residency.mirror_hash(coord) != world.chunk_hash(coord)) {
                WS_LOG_ERROR("audit", "mirror mismatch at chunk ({}, {}, {}) on frame {}",
                             coord.x, coord.y, coord.z, frame);
                ok = false;
                break;
            }
        }
        if (!ok) break;
    }

    const ResidencyStats stats = residency.stats();
    WS_LOG_INFO("audit", "resident {} chunks / {} bricks, {} KB payload of {} KB",
                stats.resident_chunks, stats.resident_bricks, stats.payload_in_use / 1024,
                stats.payload_capacity / 1024);
    WS_LOG_INFO("audit", "uploads {}  evictions {}  hit rate {:.1f}%  chunk checks {}",
                stats.uploads, stats.evictions, stats.hit_rate() * 100.0, checked);
    WS_LOG_INFO("audit", "residency update: {:.3f} ms average, {:.3f} ms worst (frame {})",
                total_update_ms / static_cast<f64>(frames), worst_update_ms,
                worst_update_frame);

    if (!ok) {
        WS_LOG_FATAL("audit", "FAILED");
        return 1;
    }
    WS_LOG_INFO("audit", "the mirror matched the world on every frame");
    return 0;
}

class Application {
public:
    int run(const Options& options);

private:
    bool create_render_target(u32 width, u32 height);
    void destroy_render_target();
    void handle_resize();
    void record_frame(f32 time_seconds);

    void build_world();
    void stream(f64 seconds);
    void update_tools(const InputState& input, bool chisel_has_wheel, bool clipboard_has_wheel,
                      f64 dt);
    void invalidate_edited_chunks(const std::vector<Op>& ops);
    void rebuild_coarse_grids();

    Options options_;
    Window window_;
    Device device_;
    Swapchain swapchain_;
    GpuProfiler profiler_;
    Hud hud_;
    ComputePipeline visibility_;
    ComputePipeline resolve_;
    GpuImage visibility_image_;
    GpuImage render_target_;
    GpuImage depth_target_;
    VkDescriptorSetLayout resolve_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet resolve_set_ = VK_NULL_HANDLE;
    Camera camera_;
    u32 debug_mode_ = 0;
    f32 detail_bias_ = 1.0f;
    bool mouse_look_ = false;
    // The click that captures the mouse must not also start a cut. Set when capture
    // happens, cleared when every button has come back up.
    bool swallow_click_ = false;

    // The chisel and its history. One player for now; the id is threaded through anyway
    // because undo is per player and retrofitting that later means revisiting every call.
    Chisel chisel_;
    Clipboard clipboard_;
    Toolbelt toolbelt_;
    KeyRepeat repeat_more_;
    KeyRepeat repeat_fewer_;
    KeyRepeat repeat_turn_[4];   // left, right, up, down
    EditHistory history_;
    OpLog op_log_;
    std::vector<VoxelTypeId> materials_;
    usize material_index_ = 0;
    u64 tick_ = 0;
    u64 last_edit_voxels_ = 0;
    f64 last_edit_ms_ = 0.0;

    // World and streaming. There is no camera yet (Stage 3), so a debug focus point
    // orbits the scene and drives the same demand-driven path the renderer will.
    TagRegistry tags_;
    PropertyRegistry properties_;
    VoxelTypeTable types_;
    World world_;
    MatterLedger ledger_;
    Updater updater_;
    ResidencyManager residency_;
    ThumbnailCache thumb_cache_;
    ThumbnailBudget thumb_budget_;
    ChunkCoord world_min_{};
    ChunkCoord world_max_{};
    bool world_bounds_valid_ = false;
    WorldBuffers world_buffers_;
    ResidencyBudget residency_budget_;
    FeedbackBuffer feedback_;
    GpuBuffer params_buffer_;
    // The clipboard's held clip, as the resolve pass sees it: one cell per voxel, holding
    // the type id plus one so that zero can mean "outside the clip". Device-local, because
    // a ghost that fills the screen is read once per pixel and reading that over the bus
    // would make the preview cost more than the world behind it.
    GpuBuffer clip_buffer_;
    GpuBuffer clip_staging_;
    struct ClipSlot {
        u32 first_cell = 0;
        u32 first_block = 0;
        u32 size[3]{};
        u32 blocks[3]{};
    };
    std::vector<ClipSlot> clip_slots_;
    u64 clip_uploaded_revision_ = 0;
    bool clip_upload_pending_ = false;
    u64 params_stride_ = 256;
    u64 frame_counter_ = 0;
    u32 last_feedback_ = 0;
    u32 last_feedback_truncated_ = 0;
    u32 last_feedback_accepted_ = 0;
    u32 last_feedback_rejected_ = 0;   // reported, but the world has no chunk there
    u32 last_thumbs_wanted_ = 0;
    u32 last_thumbs_built_ = 0;
    f64 residency_ms_ = 0.0;
    f64 worst_residency_ms_ = 0.0;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    FrameStats stats_;
};

bool Application::create_render_target(u32 width, u32 height) {
    visibility_image_ = create_storage_image(device_, width, height, VK_FORMAT_R32G32B32A32_UINT,
                                             "visibility");
    render_target_ = create_storage_image(device_, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                          "render_target");
    depth_target_ = create_storage_image(device_, width, height, VK_FORMAT_R32_SFLOAT,
                                         "depth_target");

    // Images are the only bindings that change on resize; the world buffers are created
    // once and never move.
    VkDescriptorImageInfo vis_info{};
    vis_info.imageView = visibility_image_.view;
    vis_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo depth_info{};
    depth_info.imageView = depth_target_.view;
    depth_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo colour_info{};
    colour_info.imageView = render_target_.view;
    colour_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[4]{};
    for (VkWriteDescriptorSet& write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    writes[0].dstSet = descriptor_set_;
    writes[0].dstBinding = 0;
    writes[0].pImageInfo = &vis_info;
    writes[1].dstSet = descriptor_set_;
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &depth_info;
    writes[2].dstSet = resolve_set_;
    writes[2].dstBinding = 0;
    writes[2].pImageInfo = &vis_info;
    writes[3].dstSet = resolve_set_;
    writes[3].dstBinding = 1;
    writes[3].pImageInfo = &colour_info;
    vkUpdateDescriptorSets(device_.handle(), 4, writes, 0, nullptr);
    return true;
}

void Application::destroy_render_target() {
    if (visibility_image_.valid()) destroy_image(device_, visibility_image_);
    if (render_target_.valid()) destroy_image(device_, render_target_);
    if (depth_target_.valid()) destroy_image(device_, depth_target_);
}

void Application::handle_resize() {
    // The window reports a size change on the first frame even when nothing moved;
    // rebuilding the swapchain for that is pure waste.
    if (!swapchain_.needs_recreate() && swapchain_.extent().width == window_.width() &&
        swapchain_.extent().height == window_.height()) {
        return;
    }
    device_.wait_idle();
    if (!swapchain_.recreate(window_.width(), window_.height())) return;
    destroy_render_target();
    create_render_target(swapchain_.extent().width, swapchain_.extent().height);
}

void Application::build_world() {
    const TestScenePalette palette = create_test_palette(types_, tags_);
    const u64 start = now_ns();
    build_test_scene(world_, palette, 1024, ledger_);
    world_.compact();

    // What the chisel places, cycled with Q and E. The scene's own palette for now; the
    // material picker is part of the interface stage.
    materials_ = {palette.stone,  palette.stone_light, palette.stone_dark, palette.wood,
                  palette.metal,  palette.glass,       palette.lamp};
    material_index_ = options_.material % materials_.size();
    chisel_.set_material(materials_[material_index_]);


    const WorldStats stats = world_.stats();
    WS_LOG_INFO("world",
                "test scene built in {:.0f} ms: {} chunks, {} bricks, {} solid voxels, "
                "{} MB ({:.3f} bytes/voxel)",
                ns_to_ms(now_ns() - start), stats.chunks, stats.bricks, stats.solid_voxels,
                stats.bytes / (1024 * 1024), stats.bytes_per_voxel());
}

// The chisel, and undo. Everything a player does to the world enters here and leaves as an
// op, which is the invariant the whole architecture rests on (documentation/02).
void Application::update_tools(const InputState& input, bool chisel_has_wheel,
                               bool clipboard_has_wheel, f64 dt) {
    constexpr u32 kLocalPlayer = 1;

    // A scripted edit, applied part way through the run rather than while the world is
    // being built. That is deliberate: an edit made before streaming exists proves nothing,
    // and the interesting question is whether an edit to an already-resident chunk reaches
    // the GPU. Applying it at a known frame makes that testable from a screenshot.
    constexpr u64 kScriptedEditFrame = 100;
    if (!options_.edit.empty() && frame_counter_ == kScriptedEditFrame) {
        // (the scripted clip below shares this frame number)
        i64 values[7]{0, 0, 0, 0, 0, 0, 0};
        parse_numbers(options_.edit, values, 7);
        const VoxelTypeId type =
            (values[6] <= 0) ? kAir
                             : materials_[static_cast<usize>(values[6] - 1) % materials_.size()];
        const Op op = Op::fill_box(tick_++, kLocalPlayer, values[0], values[1], values[2],
                                   values[3], values[4], values[5], type,
                                   (type == kAir) ? MatterReason::PlayerBreak
                                                  : MatterReason::PlayerPlace);
        const u64 started = now_ns();
        const OpResult result = history_.apply(world_, ledger_, op_log_, op);
        WS_LOG_INFO("chisel",
                    "scripted edit: {} voxels changed of {} visited in {:.3f} ms "
                    "(apply {:.3f}, undo capture {:.3f} into {} ops)",
                    result.voxels_changed, result.voxels_visited, ns_to_ms(now_ns() - started),
                    history_.last_apply_ms(), history_.last_capture_ms(),
                    history_.last_inverse_ops());
        if (result.voxels_changed > 0) rebuild_coarse_grids();
    }

    if (!options_.clip.empty() && frame_counter_ == kScriptedEditFrame) {
        i64 values[12]{0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
        parse_numbers(options_.clip, values, 12);
        // Whichever slot the clipboard lives on, and cycle to it if that slot holds more
        // than one tool. The belt's layout is a default, not a fact this code can assume.
        for (u32 slot = 0; slot < kToolSlots && toolbelt_.active() != ToolKind::Clipboard;
             ++slot) {
            if (!toolbelt_.select_slot(slot)) continue;
            for (u32 i = 0; i < toolbelt_.slot_size(slot); ++i) {
                if (toolbelt_.active() == ToolKind::Clipboard) break;
                toolbelt_.cycle(1);
            }
        }
        const u64 started = now_ns();
        Clip clip = capture_clip(world_, values[0], values[1], values[2], values[3], values[4],
                                 values[5]);
        const u64 solid = clip.solid_count();
        clipboard_.hold(std::move(clip), values[0], values[1], values[2]);
        // Signed: negative places the copies past the ghost instead of between.
        clipboard_.set_copies(static_cast<i32>(values[9]));
        if (values[10] != 0) clipboard_.turn(1, static_cast<f64>(values[10]));
        // Scale as a percentage, so 250 is two and a half times.
        if (values[11] != 0) clipboard_.resize(static_cast<f64>(values[11]) / 100.0);
        clipboard_.set_offset(values[6], values[7], values[8]);
        WS_LOG_INFO("clip",
                    "scripted clip: {}x{}x{} cells, {} solid, scale {:.3f}, {} copies, "
                    "set up in {:.3f} ms (bake {:.3f} ms, {} cells baked{})",
                    clipboard_.clip().size[0], clipboard_.clip().size[1],
                    clipboard_.clip().size[2], solid, clipboard_.scale()[0],
                    clipboard_.copies(), ns_to_ms(now_ns() - started),
                    clipboard_.last_bake_ms(), clipboard_.baked_cells(),
                    clipboard_.baking_truncated() ? ", TRUNCATED" : "");
    }

    if (input.was_pressed(Key::Q) && !materials_.empty()) {
        material_index_ = (material_index_ + materials_.size() - 1) % materials_.size();
        chisel_.set_material(materials_[material_index_]);
    }
    if (input.was_pressed(Key::E) && !materials_.empty()) {
        material_index_ = (material_index_ + 1) % materials_.size();
        chisel_.set_material(materials_[material_index_]);
    }

    if (input.was_pressed(Key::Z)) {
        if (history_.undo(world_, ledger_, op_log_, tick_++, kLocalPlayer)) {
            rebuild_coarse_grids();
        }
    }
    if (input.was_pressed(Key::X)) {
        if (history_.redo(world_, ledger_, op_log_, tick_++, kLocalPlayer)) {
            rebuild_coarse_grids();
        }
    }

    // The buttons only reach a tool once the mouse belongs to the world.
    const bool left = mouse_look_ && !swallow_click_ && input.mouse_left;
    const bool right = mouse_look_ && !swallow_click_ && input.mouse_right;
    const bool middle = mouse_look_ && !swallow_click_ && input.mouse_middle;

    const f64 origin[3] = {camera_.position_x(), camera_.position_y(), camera_.position_z()};
    f32 forward[3];
    camera_.forward_vector(forward);
    const f64 direction[3] = {forward[0], forward[1], forward[2]};

    std::vector<Op> ops;
    if (toolbelt_.active() == ToolKind::Clipboard) {
        ClipboardInput tool{};
        tool.left = left;
        tool.right = right;
        tool.middle = middle;
        tool.wheel = clipboard_has_wheel ? input.wheel : 0.0f;
        tool.big_step = input.is_down(Key::Shift);
        tool.adjust_distance = chisel_has_wheel;
        if (chisel_has_wheel) tool.wheel = input.wheel;
        tool.toggle_paste_mode = input.was_pressed(Key::P);
        tool.toggle_snap = input.was_pressed(Key::O);
        tool.cycle_adjust = input.was_pressed(Key::Slash);
        tool.clear_points = input.was_pressed(Key::R);
        tool.cancel = input.was_pressed(Key::Backspace);
        tool.increase = repeat_more_.poll(input.is_down(Key::Period), dt);
        tool.decrease = repeat_fewer_.poll(input.is_down(Key::Comma), dt);
        tool.turn_left = repeat_turn_[0].poll(input.is_down(Key::Left), dt);
        tool.turn_right = repeat_turn_[1].poll(input.is_down(Key::Right), dt);
        tool.turn_up = repeat_turn_[2].poll(input.is_down(Key::Up), dt);
        tool.turn_down = repeat_turn_[3].poll(input.is_down(Key::Down), dt);
        if (!clipboard_.update(world_, tool, origin, direction, dt, tick_, kLocalPlayer, ops)) {
            return;
        }
    } else {
        ChiselInput tool{};
        tool.left = left;
        tool.right = right;
        tool.middle = middle;
        tool.wheel = chisel_has_wheel ? input.wheel : 0.0f;
        tool.adjust_distance = chisel_has_wheel;
        tool.clear_points = input.was_pressed(Key::R);
        tool.cancel = input.was_pressed(Key::Backspace);
        tool.toggle_overwrite = input.was_pressed(Key::P);
        tool.toggle_anchor = input.was_pressed(Key::O);

        Op op;
        if (!chisel_.update(world_, tool, origin, direction, tick_, kLocalPlayer, op)) return;
        ops.push_back(op);
    }

    const u64 started = now_ns();
    const OpResult result = history_.apply_group(world_, ledger_, op_log_, ops);
    ++tick_;
    last_edit_ms_ = ns_to_ms(now_ns() - started);
    last_edit_voxels_ = result.voxels_changed;

    // An edit can create chunks where the world had none, or empty the last brick out of
    // one. The coarse occupancy grids describe what the world contains, so they are what
    // tells the marcher there is now something to look for here.
    if (result.voxels_changed > 0) {
        rebuild_coarse_grids();
        invalidate_edited_chunks(ops);
    }
}

// Tells streaming which chunks an edit changed.
//
// This has to be pushed, because it cannot be pulled. The renderer's feedback reports chunks
// it wanted and *could not find* — that is the whole mechanism. A chunk that is resident but
// out of date is found, so it is never reported, so it is never refreshed, and it goes on
// showing what it used to hold. Carve a room inside a hill you have already looked at and
// the hill stays solid until something unrelated evicts that chunk.
//
// The edit knows exactly which chunks it touched, so it says so.
void Application::invalidate_edited_chunks(const std::vector<Op>& ops) {
    for (const Op& raw : ops) {
        Op op = raw;
        op.normalise();
        for (i64 cz = chunk_of(op.z0); cz <= chunk_of(op.z1); ++cz) {
            for (i64 cy = chunk_of(op.y0); cy <= chunk_of(op.y1); ++cy) {
                for (i64 cx = chunk_of(op.x0); cx <= chunk_of(op.x1); ++cx) {
                    const ChunkCoord coord{cx, cy, cz};
                    // invalidate(), not request(): a request lives for one frame and is
                    // dropped when that frame's upload budget runs out. A large edit touches
                    // hundreds of chunks and the budget serves four, so all but the first
                    // four were being forgotten — and nothing ever asked again, because a
                    // stale chunk is one the renderer can still find.
                    if (!world_.has_chunk(coord)) continue;
                    residency_.invalidate(coord);
                    // The thumbnail is a summary of contents, so changing the contents makes
                    // it wrong too — and it is what the same chunk draws as from a distance.
                    thumb_cache_.invalidate(coord);
                }
            }
        }
    }
}

// Stands in for the renderer's feedback buffer until Stage 3: request every chunk within
// a radius of a moving focus point. The residency manager cannot tell the difference, and
// neither can the eviction path, which is the point of exercising it now.
// Rebuilds the world-occupancy grids around wherever the camera is now.
//
// Level 0 of those grids — the one that answers "the world has a chunk here and you do not
// have it" — is only recorded within a window around this point, because that is what stops
// two distant regions colliding in the wrapped grid and inventing chunks that do not exist.
// So the grids have to follow the camera, not only world edits.
void Application::rebuild_coarse_grids() {
    residency_.rebuild_coarse(
        world_, ChunkCoord{camera_.chunk_x(), camera_.chunk_y(), camera_.chunk_z()});
    // Every path that changes which chunks exist already comes through here, which makes it
    // the one place the thumbnail cache needs telling that its work list is stale.
    thumb_cache_.mark_world_changed();

    // And the one place to note how far the world reaches, which is how far a ray can
    // usefully travel now that thumbnails draw well past what is resident.
    world_bounds_valid_ = false;
    world_.for_each_chunk([this](const ChunkCoord& coord, const Chunk& chunk) {
        if (chunk.empty()) return;
        if (!world_bounds_valid_) {
            world_min_ = coord;
            world_max_ = coord;
            world_bounds_valid_ = true;
            return;
        }
        world_min_.x = std::min(world_min_.x, coord.x);
        world_min_.y = std::min(world_min_.y, coord.y);
        world_min_.z = std::min(world_min_.z, coord.z);
        world_max_.x = std::max(world_max_.x, coord.x);
        world_max_.y = std::max(world_max_.y, coord.y);
        world_max_.z = std::max(world_max_.z, coord.z);
    });
}

void Application::stream(f64 seconds) {
    (void)seconds;

    // Keep level 0 centred on the camera. Rebuilt on a margin rather than on every chunk
    // boundary, so walking to and fro across one does not rebuild twice a second. The
    // margin is well inside the window, so the grid is never consulted past what it
    // describes.
    const ChunkCoord centre{camera_.chunk_x(), camera_.chunk_y(), camera_.chunk_z()};
    const ChunkCoord& built = residency_.coarse_centre();
    constexpr i64 kCoarseFollowMargin = 8;   // chunks, 64 m
    if (std::abs(centre.x - built.x) > kCoarseFollowMargin ||
        std::abs(centre.y - built.y) > kCoarseFollowMargin ||
        std::abs(centre.z - built.z) > kCoarseFollowMargin) {
        rebuild_coarse_grids();
    }

    // What the renderer asked for, two frames ago. This is the rule that makes residency
    // follow the *view* rather than the camera position — a chunk 300 m away that covers
    // a hundred pixels gets streamed; one 10 m away behind a wall does not.
    const std::vector<FeedbackEntry>& wanted = feedback_.read(swapchain_.frame_index());
    last_feedback_ = feedback_.last_reported();
    last_feedback_truncated_ = feedback_.last_truncated();
    u32 accepted = 0;
    u32 rejected = 0;
    for (const FeedbackEntry& entry : wanted) {
        const ChunkCoord coord{entry.x, entry.y, entry.z};
        if (!world_.has_chunk(coord)) {
            // Reported, but the world has nothing there. A few of these are normal — the
            // grid answers at block granularity, so a ray inside an occupied block still
            // asks about individual chunks that turn out to be empty.
            //
            // A *large* number is a stall, and it used to be a silent one. A ray reports
            // only its nearest miss, so a phantom hides the real chunk behind it forever:
            // streaming asks for the same nothing every frame while the world stays
            // unloaded. It is counted so it can be seen rather than deduced.
            ++rejected;
            continue;
        }
        residency_.request(coord);
        ++accepted;

        // Also pull in the immediate neighbours. A ray reports one chunk, and only the
        // chunks some ray happened to land on would ever be requested — which left visible
        // notches along the edges of what had streamed. Dilating by one face covers the
        // gaps between sample points and costs nothing: neighbours that are already
        // resident are a hit, and ones the world does not have are one lookup.
        const ChunkCoord neighbours[6] = {
            {coord.x - 1, coord.y, coord.z}, {coord.x + 1, coord.y, coord.z},
            {coord.x, coord.y - 1, coord.z}, {coord.x, coord.y + 1, coord.z},
            {coord.x, coord.y, coord.z - 1}, {coord.x, coord.y, coord.z + 1},
        };
        for (const ChunkCoord& neighbour : neighbours) {
            if (world_.has_chunk(neighbour)) residency_.request(neighbour);
        }
    }
    last_feedback_accepted_ = accepted;
    last_feedback_rejected_ = rejected;

    // A small radius around the camera on top, so the ground under your feet is resident
    // before it has been looked at. Feedback cannot report what has never been on screen.
    const i64 radius_chunks = 2;
    for (i64 z = -radius_chunks; z <= radius_chunks; ++z) {
        for (i64 y = -1; y <= 1; ++y) {
            for (i64 x = -radius_chunks; x <= radius_chunks; ++x) {
                const ChunkCoord coord{centre.x + x, centre.y + y, centre.z + z};
                if (world_.has_chunk(coord)) residency_.request(coord);
            }
        }
    }
}

void Application::record_frame(f32 time_seconds) {
    const VkCommandBuffer cmd = swapchain_.cmd();
    const VkExtent2D extent = swapchain_.extent();

    profiler_.begin_frame(cmd, swapchain_.frame_index());
    feedback_.begin_frame(cmd);

    // ---- streaming ------------------------------------------------------------------
    profiler_.begin_pass(cmd, "streaming", 0.8);
    stream(static_cast<f64>(time_seconds));
    const u64 residency_start = now_ns();
    const UploadBatch& batch = residency_.update(world_, frame_counter_);
    residency_ms_ = ns_to_ms(now_ns() - residency_start);
    if (residency_ms_ > worst_residency_ms_) worst_residency_ms_ = residency_ms_;
    world_buffers_.upload(cmd, residency_, batch, swapchain_.frame_index());
    // If the occupancy grid did not fit in this frame's staging, ask for it again. Clearing
    // the dirty flag over an upload that never happened is what left holes in the world.
    if (world_buffers_.stats().coarse_incomplete) residency_.mark_coarse_dirty();

    // The other tier. Pushed from the camera rather than pulled from the view, so it never
    // waits on a round trip and cannot deadlock on what a ray did or did not reach.
    {
        const ChunkCoord centre{camera_.chunk_x(), camera_.chunk_y(), camera_.chunk_z()};
        const ThumbnailBatch& thumbs = thumb_cache_.update(world_, centre, frame_counter_);
        if (!world_buffers_.upload_thumbnails(cmd, thumb_cache_, thumbs)) {
            thumb_cache_.defer_last_batch();
        }
        last_thumbs_wanted_ = thumbs.wanted;
        last_thumbs_built_ = thumbs.built;
    }

    world_buffers_.upload_tables(cmd, types_);
    profiler_.add_bytes(world_buffers_.stats().staged_bytes);
    profiler_.end_pass(cmd);

    {
        const ResidencyStats residency = residency_.stats();
        const WorldBufferStats buffers = world_buffers_.stats();
        StreamingReport report;
        report.world_chunks = world_.chunk_count();
        report.resident_chunks = residency.resident_chunks;
        report.resident_bricks = residency.resident_bricks;
        report.resident_bytes = residency.total_bytes;
        report.payload_in_use = residency.payload_in_use;
        report.payload_capacity = residency.payload_capacity;
        report.staged_bytes = buffers.staged_bytes;
        report.deferred_bytes = buffers.deferred_bytes;
        report.copy_regions = buffers.copy_regions;
        report.raw_regions = buffers.raw_regions;
        report.evictions = residency.evictions;
        report.hit_rate = residency.hit_rate();
        report.out_of_memory = batch.out_of_memory;
        report.update_ms = residency_ms_;
        report.worst_update_ms = worst_residency_ms_;
        report.feedback_reports = last_feedback_;
        report.feedback_dropped = last_feedback_truncated_;
        hud_.set_streaming(report);
        if (options_.stream_log && (frame_counter_ % 60 == 0)) {
            WS_LOG_INFO("diag",
                        "f{} resident {}/{} added {} refreshed {} evicted {} deferred {} "
                        "bricks {} oom {} feedback {} accepted {} phantom {} thumbs {} want {}",
                        frame_counter_, report.resident_chunks, report.world_chunks,
                        batch.chunks_added, batch.chunks_refreshed, batch.chunks_evicted,
                        batch.chunks_deferred, report.resident_bricks,
                        batch.out_of_memory ? 1 : 0, last_feedback_, last_feedback_accepted_,
                        last_feedback_rejected_, thumb_cache_.resident_count(),
                        last_thumbs_wanted_);
        }

        const ChiselPreview& preview = chisel_.preview();
        const ClipboardPreview& ghost = clipboard_.preview();
        const bool clipboard_active = toolbelt_.active() == ToolKind::Clipboard;

        ToolReport tool;
        tool.active_tool = tool_name(toolbelt_.active());
        tool.active_slot = toolbelt_.active_slot();
        for (u32 slot = 0; slot < kToolSlots; ++slot) {
            tool.slot_count[slot] = toolbelt_.slot_size(slot);
            tool.slot_position[slot] = toolbelt_.slot_position(slot);
        }

        if (clipboard_active) {
            tool.active = ghost.selecting || ghost.holding;
            tool.dragging = ghost.selecting;
            tool.holding = ghost.holding;
            tool.too_large = ghost.too_large;
            tool.copies = clipboard_.copies();
            tool.paste_mode = paste_mode_name(clipboard_.paste_mode());
            tool.adjust_mode = adjust_mode_name(clipboard_.adjust_mode());
            {
                const f64* s = clipboard_.scale();
                const bool uniform = std::abs(s[0] - s[1]) < 1e-6 && std::abs(s[1] - s[2]) < 1e-6;
                if (uniform) {
                    std::snprintf(tool.scale_text, sizeof(tool.scale_text), "%.3gx", s[0]);
                } else {
                    std::snprintf(tool.scale_text, sizeof(tool.scale_text), "%.2g/%.2g/%.2g",
                                  s[0], s[1], s[2]);
                }
            }
            tool.grid_snap = clipboard_.grid_snap();
            tool.turn_step = clipboard_.turn_step_degrees();
            for (int axis = 0; axis < 3; ++axis) tool.angles[axis] = clipboard_.angles_degrees()[axis];
            if (ghost.holding && ghost.instances > 0) {
                for (int axis = 0; axis < 3; ++axis) {
                    tool.min[axis] = ghost.min[0][axis];
                    tool.max[axis] = ghost.max[0][axis];
                }
                tool.volume = clipboard_.clip().cell_count();
            } else {
                for (int axis = 0; axis < 3; ++axis) {
                    tool.min[axis] = ghost.select_min[axis];
                    tool.max[axis] = ghost.select_max[axis];
                }
            }
        } else {
            tool.active = preview.active;
            tool.dragging = preview.dragging;
            tool.carving = preview.mode == ChiselMode::Carve;
            tool.overwrite = chisel_.overwrites();
            tool.against_face = chisel_.places_against_face();
            for (int axis = 0; axis < 3; ++axis) {
                tool.min[axis] = preview.min[axis];
                tool.max[axis] = preview.max[axis];
            }
            tool.volume = preview.volume;
        }
        tool.snapping = chisel_.snapping();
        tool.distance_metres = chisel_.distance() / kVoxelsPerMetre;
        tool.constraints = static_cast<u32>(chisel_.constraints().size());
        tool.undo_depth = history_.undo_depth(1);
        tool.redo_depth = history_.redo_depth(1);
        tool.history_bytes = history_.bytes();
        tool.last_edit_voxels = last_edit_voxels_;
        tool.last_edit_ms = last_edit_ms_;
        tool.ops_logged = op_log_.size();
        hud_.set_tool(tool);

        UpdateReport update;
        switch (updater_.state()) {
            case UpdateState::Available:
                update.show = true;
                update.offering = true;
                update.headline = "WorldShaper " + updater_.info().tag + " is available";
                break;
            case UpdateState::Downloading:
                update.show = true;
                update.downloading = true;
                update.progress = updater_.progress();
                update.headline = "downloading " + updater_.info().tag;
                break;
            case UpdateState::Installed:
                update.show = true;
                update.headline = updater_.message();
                break;
            default:
                break;
        }
        hud_.set_update(update);
    }
    ++frame_counter_;

    // ---- the clipboard's held clip, if it has changed --------------------------------
    //
    // Uploaded only when its *contents* move — a new selection or a rotation. Sliding the
    // ghost or fanning out copies changes nothing here, because those are instance origins
    // in the parameter block, not voxels.
    if (clipboard_.revision() != clip_uploaded_revision_) {
        clip_uploaded_revision_ = clipboard_.revision();
        clip_slots_.clear();
        const std::vector<Clip>& shapes = clipboard_.shapes();

        u32* out = static_cast<u32*>(clip_staging_.mapped);
        u64 cursor = 0;
        for (const Clip& shape : shapes) {
            const u64 cells = shape.cell_count();
            const u64 blocks = shape.coarse_count();
            if (shape.empty() || cursor + cells + blocks > kMaxClipPoolCells) break;
            for (u64 i = 0; i < cells; ++i) {
                // Type plus one, so that zero can mean "not part of the clip" without
                // stealing a type id or costing a second array.
                out[cursor + i] = (shape.inside[i] != 0) ? (shape.voxels[i] + 1u) : 0u;
            }
            for (u64 i = 0; i < blocks; ++i) {
                out[cursor + cells + i] = shape.coarse[i];
            }
            ClipSlot slot;
            slot.first_cell = static_cast<u32>(cursor);
            slot.first_block = static_cast<u32>(cursor + cells);
            for (u32 axis = 0; axis < 3; ++axis) {
                slot.size[axis] = static_cast<u32>(shape.size[axis]);
                slot.blocks[axis] = static_cast<u32>(shape.coarse_size[axis]);
            }
            clip_slots_.push_back(slot);
            cursor += cells + blocks;
        }

        if (cursor > 0) {
            clip_upload_pending_ = true;
            WS_LOG_INFO("clip", "uploaded {} shape(s), {} cells ({} KB) for the ghost",
                        clip_slots_.size(), cursor, (cursor * sizeof(u32)) / 1024);
            VkBufferCopy copy{};
            copy.size = cursor * sizeof(u32);
            vkCmdCopyBuffer(cmd, clip_staging_.buffer, clip_buffer_.buffer, 1, &copy);
            profiler_.add_bytes(copy.size);
        }
    }
    if (clip_upload_pending_) {
        clip_upload_pending_ = false;
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    for (const GpuImage* image : {&visibility_image_, &render_target_, &depth_target_}) {
        image_barrier(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    // ---- frame parameters -----------------------------------------------------------
    (void)time_seconds;
    RenderParams params{};
    params.origin[0] = camera_.local_x();
    params.origin[1] = camera_.local_y();
    params.origin[2] = camera_.local_z();
    camera_.forward_vector(params.forward);
    camera_.right_vector(params.right);
    camera_.up_vector(params.up);
    params.camera_chunk[0] = static_cast<i32>(camera_.chunk_x());
    params.camera_chunk[1] = static_cast<i32>(camera_.chunk_y());
    params.camera_chunk[2] = static_cast<i32>(camera_.chunk_z());
    params.grid_dims[0] = residency_budget_.grid_width;
    params.grid_dims[1] = residency_budget_.grid_height;
    params.grid_dims[2] = residency_budget_.grid_depth;
    // Clip rays to what is resident, plus a margin.
    //
    // The margin is not slack, it is the mechanism: feedback can only report chunks a ray
    // actually reached, so clipping tightly to the resident set means rays can never
    // discover anything outside it and streaming deadlocks — the first version of this
    // rendered nothing at all for exactly that reason. With a margin the frontier
    // advances by that many chunks per frame until it covers whatever is visible.
    constexpr i64 kExploreMargin = 24;   // chunks, so ~190 m of frontier per frame
    ChunkCoord bounds_lo{};
    ChunkCoord bounds_hi{};
    if (residency_.resident_bounds(bounds_lo, bounds_hi)) {
        params.bounds_min[0] = static_cast<i32>(bounds_lo.x - camera_.chunk_x() - kExploreMargin);
        params.bounds_min[1] = static_cast<i32>(bounds_lo.y - camera_.chunk_y() - kExploreMargin);
        params.bounds_min[2] = static_cast<i32>(bounds_lo.z - camera_.chunk_z() - kExploreMargin);
        params.bounds_max[0] = static_cast<i32>(bounds_hi.x - camera_.chunk_x() + kExploreMargin);
        params.bounds_max[1] = static_cast<i32>(bounds_hi.y - camera_.chunk_y() + kExploreMargin);
        params.bounds_max[2] = static_cast<i32>(bounds_hi.z - camera_.chunk_z() + kExploreMargin);
    } else {
        // Nothing resident yet: no box to clip to, so let the rays look around.
        for (u32 axis = 0; axis < 3; ++axis) {
            params.bounds_min[axis] = -256;
            params.bounds_max[axis] = 256;
        }
    }

    // Then widen it to the world's own extent, because thumbnails draw well past what is
    // resident. The box was written when a chunk had to be fully resident to appear at all,
    // so clipping to the resident set plus a margin lost nothing; it does now. A thumbnail a
    // kilometre away is real geometry, and a ray has to be allowed to reach it.
    //
    // The world's extent, and not simply a large number: a large number measured 23.7 ms
    // against 0.78 on the test scene. The clip is not slack — it is what stops a ray that
    // will never hit anything from stepping until its budget runs out. Bounded by the world,
    // a ray leaving it stops at the edge, which is both exact and free.
    if (world_bounds_valid_) {
        const i64 lo[3] = {world_min_.x - camera_.chunk_x(), world_min_.y - camera_.chunk_y(),
                           world_min_.z - camera_.chunk_z()};
        const i64 hi[3] = {world_max_.x - camera_.chunk_x(), world_max_.y - camera_.chunk_y(),
                           world_max_.z - camera_.chunk_z()};
        for (u32 axis = 0; axis < 3; ++axis) {
            params.bounds_min[axis] =
                std::min<i32>(params.bounds_min[axis], static_cast<i32>(lo[axis]));
            params.bounds_max[axis] =
                std::max<i32>(params.bounds_max[axis], static_cast<i32>(hi[axis]));
        }
    }

    params.resolution[0] = extent.width;
    params.resolution[1] = extent.height;
    params.resolution[2] = debug_mode_;
    params.resolution[3] = kFeedbackCapacity;
    params.lens[0] = camera_.tan_half_fov();
    // Continuous detail plus the resident-bounds clip mean distance costs almost nothing,
    // so this is set past anything a world will contain rather than being a quality knob.
    params.lens[1] = 4000000.0f;   // voxels: 125 km
    params.lens[2] = detail_bias_;
    params.thumb_dims[0] = static_cast<i32>(thumb_budget_.grid_width);
    params.thumb_dims[1] = static_cast<i32>(thumb_budget_.grid_height);
    params.thumb_dims[2] = static_cast<i32>(thumb_budget_.grid_depth);
    params.thumb_dims[3] = 0;

    // The chisel's preview, moved into the camera-relative space the shader works in. The
    // camera chunk corner is the origin of that space, so the same subtraction that the
    // ray origin gets applies here.
    {
        const i64 base[3] = {camera_.chunk_x() * kChunkEdge, camera_.chunk_y() * kChunkEdge,
                             camera_.chunk_z() * kChunkEdge};
        auto set_tint = [](f32 out[4], const VisualRecord& record, bool invert) {
            const f32 r = static_cast<f32>(record.red) / 255.0f;
            const f32 g = static_cast<f32>(record.green) / 255.0f;
            const f32 b = static_cast<f32>(record.blue) / 255.0f;
            out[0] = invert ? 1.0f - r : r;
            out[1] = invert ? 1.0f - g : g;
            out[2] = invert ? 1.0f - b : b;
            out[3] = 1.0f;
        };

        u32 box = 0;
        const auto add_box = [&](const i64 lo[3], const i64 hi[3], i32 state,
                                 bool outline = true) {
            if (box >= kMaxPreviewBoxes) return;
            for (int axis = 0; axis < 3; ++axis) {
                params.box_min[box][axis] = static_cast<i32>(lo[axis] - base[axis]);
                params.box_max[box][axis] = static_cast<i32>(hi[axis] - base[axis]);
            }
            params.box_min[box][3] = state;
            params.box_max[box][3] = outline ? 1 : 0;
            ++box;
        };

        if (toolbelt_.active() == ToolKind::Clipboard) {
            const ClipboardPreview& ghost = clipboard_.preview();
            if (ghost.selecting) {
                add_box(ghost.select_min, ghost.select_max, ghost.too_large ? 3 : 1);
            }
            // State 5: march the clip inside this box rather than outlining it, so the
            // ghost shows the voxels that are about to land instead of the space they will
            // land in. Falls back to an outline when the clip is too large to upload.
            for (u32 n = 0; n < ghost.instances && n < kMaxPreviewInstances; ++n) {
                const u32 shape = ghost.shape[n];
                const bool voxel_ghost = shape < clip_slots_.size();
                // Only the last copy — the one the wheel is steering — gets an outline.
                // Sixteen wireframes is both noise to look at and six plane tests a pixel
                // each; the voxels are what says where the others are.
                const bool outline = !voxel_ghost || (n + 1 == ghost.instances);
                const u32 index = box;
                add_box(ghost.min[n], ghost.max[n], voxel_ghost ? 5 : 2, outline);
                if (voxel_ghost && index < kMaxPreviewBoxes) {
                    const ClipSlot& used = clip_slots_[shape];
                    params.clip_slot[index][0] = used.first_cell;
                    params.clip_coarse[index][0] = used.first_block;
                    for (u32 axis = 0; axis < 3; ++axis) {
                        params.clip_slot[index][axis + 1] = used.size[axis];
                        params.clip_coarse[index][axis + 1] = used.blocks[axis];
                    }
                }
            }
            // A ghost is a copy of real voxels rather than one material, so there is no
            // single colour to outline it in. Both tints stay at zero, which the shader
            // reads as "invert the backdrop" — the neutral answer, and the one that reads
            // over anything.
        } else {
            const ChiselPreview& preview = chisel_.preview();
            if (preview.active) {
                // Idle — just the voxel under the crosshair — is not a decision yet, so it
                // gets its own state and stays neutral.
                add_box(preview.min, preview.max,
                        (preview.mode == ChiselMode::Carve)
                            ? 1
                            : ((preview.mode == ChiselMode::Place) ? 2 : 4));

                if (preview.mode == ChiselMode::Place) {
                    // The material's own colour in the open, its inverse where it is buried.
                    set_tint(params.tint_visible, types_.visual_of(chisel_.material()), false);
                    set_tint(params.tint_occluded, types_.visual_of(chisel_.material()), true);
                } else if (preview.mode == ChiselMode::Carve && preview.removing != kAir) {
                    // Inverted backdrop in the open (w stays 0), the doomed material where
                    // it is buried.
                    set_tint(params.tint_occluded, types_.visual_of(preview.removing), false);
                }
                // Everything else leaves both at zero, which the shader reads as "invert
                // the backdrop" — the right answer when there is no material to speak of.
            }
        }
        if (!options_.preview.empty()) {
            i64 values[7]{0, 0, 0, 0, 0, 0, 2};
            parse_numbers(options_.preview, values, 7);
            const i64 lo[3] = {values[0], values[1], values[2]};
            const i64 hi[3] = {values[3], values[4], values[5]};
            box = 0;   // the scripted box replaces whatever the tool wanted to show
            add_box(lo, hi, static_cast<i32>(values[6]));
            // Same colour rules as the live tool, so a scripted screenshot shows what a
            // player would see rather than a stand-in.
            if (values[6] == 2 && !materials_.empty()) {
                set_tint(params.tint_visible, types_.visual_of(chisel_.material()), false);
                set_tint(params.tint_occluded, types_.visual_of(chisel_.material()), true);
            } else if (values[6] == 1 && !materials_.empty()) {
                set_tint(params.tint_occluded, types_.visual_of(materials_[0]), false);
            }
        }
        u32 slot = 0;
        for (const std::array<i64, 3>& point : chisel_.constraints()) {
            if (slot >= kMaxPreviewMarks) break;
            params.marks[slot][0] = static_cast<i32>(point[0] - base[0]);
            params.marks[slot][1] = static_cast<i32>(point[1] - base[1]);
            params.marks[slot][2] = static_cast<i32>(point[2] - base[2]);
            params.marks[slot][3] = 1;
            ++slot;
        }
    }

    // The slot stride is the device's alignment, not sizeof — the dynamic offset passed at
    // bind time uses the aligned stride, so writing at sizeof would put frame 1's data
    // where the shader is not looking.
    std::memcpy(static_cast<u8*>(params_buffer_.mapped) +
                    static_cast<usize>(swapchain_.frame_index()) * params_stride_,
                &params, sizeof(RenderParams));

    // ---- primary visibility ---------------------------------------------------------
    profiler_.begin_pass(cmd, "visibility", 9.5);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visibility_.pipeline());
    const u32 params_offset =
        static_cast<u32>(swapchain_.frame_index()) * static_cast<u32>(params_stride_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visibility_.layout(), 0, 1,
                            &descriptor_set_, 1, &params_offset);

    vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    profiler_.add_bytes(static_cast<u64>(extent.width) * extent.height * 20);
    profiler_.end_pass(cmd);

    // ---- resolve --------------------------------------------------------------------
    VkMemoryBarrier2 vis_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    vis_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vis_barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    vis_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vis_barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo vis_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    vis_dependency.memoryBarrierCount = 1;
    vis_dependency.pMemoryBarriers = &vis_barrier;
    vkCmdPipelineBarrier2(cmd, &vis_dependency);

    profiler_.begin_pass(cmd, "resolve", 0.8);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_.pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_.layout(), 0, 1,
                            &resolve_set_, 1, &params_offset);
    vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    profiler_.add_bytes(static_cast<u64>(extent.width) * extent.height * 20);
    profiler_.end_pass(cmd);

    // Hand this frame's "what I could not find" list back to the CPU. Without this the
    // shader's report is written and then thrown away, and streaming never learns
    // anything — which is exactly what happened until it was noticed.
    feedback_.end_frame(cmd, swapchain_.frame_index());

    // ---- present ------------------------------------------------------------------
    image_barrier(cmd, render_target_.image, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    image_barrier(cmd, swapchain_.current_image(), VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);

    profiler_.begin_pass(cmd, "blit", 0.4);
    VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = {static_cast<i32>(render_target_.extent.width),
                            static_cast<i32>(render_target_.extent.height), 1};
    region.dstOffsets[1] = {static_cast<i32>(extent.width), static_cast<i32>(extent.height), 1};

    VkBlitImageInfo2 blit{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    blit.srcImage = render_target_.image;
    blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blit.dstImage = swapchain_.current_image();
    blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blit.regionCount = 1;
    blit.pRegions = &region;
    blit.filter = VK_FILTER_LINEAR;
    vkCmdBlitImage2(cmd, &blit);
    profiler_.end_pass(cmd);

    // Leave the render target in GENERAL. A frame that ends with an image in a transfer
    // layout is a trap for anything that reads it afterwards — which is exactly what the
    // screenshot path walked into.
    image_barrier(cmd, render_target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);

    image_barrier(cmd, swapchain_.current_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    profiler_.begin_pass(cmd, "hud", 0.6);
    hud_.render(cmd, swapchain_.current_view(), extent);
    profiler_.end_pass(cmd);

    image_barrier(cmd, swapchain_.current_image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

    profiler_.end_frame(cmd);
}

int Application::run(const Options& options) {
    options_ = options;

    const std::string title = std::string("WorldShaper v") + kVersion;
    if (!window_.create(title, options_.width, options_.height,
                        options_.size_explicit)) {
        return 1;
    }
    if (!device_.create(&window_, options_.validation)) return 1;
    if (!swapchain_.create(device_, window_.width(), window_.height(), options_.vsync)) {
        return 1;
    }
    if (!profiler_.create(device_)) return 1;

    build_world();

    // Sized from detected VRAM, and never resized afterwards
    // (documentation/03-voxel-data-model.md Â§8).
    const u64 vram = device_.caps().device_local_bytes;
    const u64 vram_budget = (vram >= (8ull << 30))   ? (1ull << 30)
                            : (vram >= (4ull << 30)) ? (384ull << 20)
                                                     : (192ull << 20);

    // A brick costs two separate things, and they have to be budgeted separately.
    //
    //   its **payload** — the packed voxel indices and palette, anywhere from 8 bytes for a
    //   uniform brick to 2 KB for one where every voxel differs
    //   its **slot** — a header and a 64-byte occupancy mask, the same for every brick
    //
    // The slot count used to be derived from the payload budget as `payload / 1024`, on the
    // assumption that a brick averages about a kilobyte. That assumption fails in exactly the
    // case that matters: a large flat build is almost entirely *uniform* bricks, which cost
    // eight bytes of payload each and a full slot each. Filling a 3 km square with one
    // material used 8 MB of a 1 GB payload budget and ran clean out of slots at 128 chunks —
    // whereupon residency evicted a chunk for every chunk it added, and the world blinked in
    // and out as you moved. The payload gauge said 1% and everything looked fine.
    //
    // So: split the budget, and size the slots by what a slot actually costs.
    //
    // Thumbnails take a slice off the top, because they buy something the other two cannot
    // buy at any price. Full detail is bounded by memory however it is split: a chunk has to
    // be entirely resident to draw at all, so past that bound the world is not drawn coarsely,
    // it is simply not drawn. A thumbnail is two kilobytes, so the same memory that holds a
    // few hundred chunks at full detail holds tens of thousands of them at a metre — which is
    // the difference between a view that ends and one that does not.
    constexpr u64 kSlotBytes = sizeof(GpuBrickHeader) + kBrickWords * sizeof(u64);
    const u64 thumb_bytes = vram_budget * 15 / 100;
    residency_budget_.payload_bytes = vram_budget * 45 / 100;
    residency_budget_.max_bricks =
        static_cast<u32>((vram_budget * 40 / 100) / kSlotBytes);
    residency_budget_.max_chunk_uploads_per_frame = 8;
    residency_budget_.max_bricks_per_frame = 8192;
    residency_.create(residency_budget_, types_);

    constexpr u64 kThumbBytes = kThumbSlotWords * sizeof(u32);
    thumb_budget_.max_thumbs = static_cast<u32>(thumb_bytes / kThumbBytes);
    thumb_cache_.create(thumb_budget_, types_);
    WS_LOG_INFO("app", "thumbnails: {} slots, {} MB, radius {} chunks ({} m)",
                thumb_budget_.max_thumbs, (thumb_budget_.max_thumbs * kThumbBytes) >> 20,
                thumb_budget_.radius_chunks, thumb_budget_.radius_chunks * 8);
    // Coarse occupancy comes from the world, so the marcher can tell "nothing here" from
    // "something here that you have not streamed yet". Without it, feedback never fires.
    rebuild_coarse_grids();
    if (!world_buffers_.create(device_, residency_budget_, thumb_budget_, 32ull << 20)) return 1;
    if (!feedback_.create(device_)) return 1;

    // One slot per frame in flight, aligned to whatever the device demands, so writing
    // next frame's parameters cannot disturb the frame still executing.
    const u64 alignment = device_.caps().min_uniform_offset;
    params_stride_ = ((sizeof(RenderParams) + alignment - 1) / alignment) * alignment;
    params_buffer_ = create_staging_buffer(device_, params_stride_ * kFramesInFlight,
                                           "render params",
                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    const u64 clip_bytes = kMaxClipPoolCells * sizeof(u32);
    clip_buffer_ = create_device_buffer(device_, clip_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        "clip cells");
    clip_staging_ = create_staging_buffer(device_, clip_bytes, "clip staging");

    // Visibility set: bindings 0-1 are the output images; 2-8 are the world, in the order
    // the marcher walks them. See shaders/visibility.comp.
    constexpr u32 kImageBindings = 2;
    constexpr u32 kBufferBindings = 11;   // 10 world buffers plus the feedback buffer
    VkDescriptorSetLayoutBinding bindings[kImageBindings + kBufferBindings + 1]{};
    for (u32 i = 0; i < kImageBindings + kBufferBindings + 1; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            (i < kImageBindings) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
            : (i < kImageBindings + kBufferBindings)
                ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = kImageBindings + kBufferBindings + 1;
    layout_info.pBindings = bindings;
    WS_VK(vkCreateDescriptorSetLayout(device_.handle(), &layout_info, nullptr, &set_layout_));

    // Resolve set: the visibility image in, the colour image out, plus the two tables it
    // needs to turn a voxel type into a colour.
    VkDescriptorSetLayoutBinding resolve_bindings[6]{};
    for (u32 i = 0; i < 6; ++i) {
        resolve_bindings[i].binding = i;
        // 0-1 images, 2-3 the type and visual tables, 4 the parameter block, 5 the
        // clipboard's held clip.
        resolve_bindings[i].descriptorType =
            (i < 2)   ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
            : (i < 4) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
            : (i == 4) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                       : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        resolve_bindings[i].descriptorCount = 1;
        resolve_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo resolve_layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    resolve_layout_info.bindingCount = 6;
    resolve_layout_info.pBindings = resolve_bindings;
    WS_VK(vkCreateDescriptorSetLayout(device_.handle(), &resolve_layout_info, nullptr,
                                      &resolve_layout_));

    const VkDescriptorPoolSize pool_sizes[]{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 24},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4},
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    WS_VK(vkCreateDescriptorPool(device_.handle(), &pool_info, nullptr, &descriptor_pool_));

    VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_alloc.descriptorPool = descriptor_pool_;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &set_layout_;
    WS_VK(vkAllocateDescriptorSets(device_.handle(), &set_alloc, &descriptor_set_));

    VkDescriptorSetAllocateInfo resolve_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    resolve_alloc.descriptorPool = descriptor_pool_;
    resolve_alloc.descriptorSetCount = 1;
    resolve_alloc.pSetLayouts = &resolve_layout_;
    WS_VK(vkAllocateDescriptorSets(device_.handle(), &resolve_alloc, &resolve_set_));

    create_render_target(swapchain_.extent().width, swapchain_.extent().height);

    // Compiled shaders sit beside the executable so a copied build folder is
    // self-contained. The source tree location comes from the build, so hot reload works
    // no matter what the working directory is.
    const std::filesystem::path spirv =
        std::filesystem::path(WS_EXE_SHADER_DIR) / "visibility.comp.spv";
    const std::filesystem::path source =
        std::filesystem::path(WS_SHADER_SOURCE_DIR) / "visibility.comp";

    if (!visibility_.create(device_, source, spirv, set_layout_, 0)) {
        WS_LOG_FATAL("app", "could not create the visibility pipeline: {}",
                     visibility_.last_error());
        return 1;
    }

    const std::filesystem::path resolve_spirv =
        std::filesystem::path(WS_EXE_SHADER_DIR) / "resolve.comp.spv";
    const std::filesystem::path resolve_source =
        std::filesystem::path(WS_SHADER_SOURCE_DIR) / "resolve.comp";
    if (!resolve_.create(device_, resolve_source, resolve_spirv, resolve_layout_,
                         0)) {
        WS_LOG_FATAL("app", "could not create the resolve pipeline: {}",
                     resolve_.last_error());
        return 1;
    }

    // The world buffers never move, so they are bound once.
    const VkBuffer marcher_buffers[]{
        world_buffers_.grid(),     world_buffers_.records(),   world_buffers_.masks(),
        world_buffers_.prefixes(), world_buffers_.headers(),   world_buffers_.occupancy(),
        world_buffers_.payload(), world_buffers_.coarse(), world_buffers_.thumb_grid(),
        world_buffers_.thumbs(), feedback_.buffer(),
    };
    static_assert(kBufferBindings == 11, "marcher buffer list must match the binding count");
    const VkBuffer resolve_buffers[]{world_buffers_.types(), world_buffers_.visuals(),
                                     clip_buffer_.buffer};

    // The parameter block, bound to both sets with a dynamic offset chosen per frame.
    VkDescriptorBufferInfo params_info{};
    params_info.buffer = params_buffer_.buffer;
    params_info.offset = 0;
    params_info.range = sizeof(RenderParams);
    VkWriteDescriptorSet params_writes[2]{};
    for (u32 i = 0; i < 2; ++i) {
        params_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        params_writes[i].dstSet = (i == 0) ? descriptor_set_ : resolve_set_;
        params_writes[i].dstBinding = (i == 0) ? (kImageBindings + kBufferBindings) : 4;
        params_writes[i].descriptorCount = 1;
        params_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        params_writes[i].pBufferInfo = &params_info;
    }
    vkUpdateDescriptorSets(device_.handle(), 2, params_writes, 0, nullptr);

    constexpr u32 kResolveBuffers = 3;   // types, visuals, clip cells
    VkDescriptorBufferInfo buffer_infos[kBufferBindings + kResolveBuffers]{};
    VkWriteDescriptorSet buffer_writes[kBufferBindings + kResolveBuffers]{};
    for (u32 i = 0; i < kBufferBindings + kResolveBuffers; ++i) {
        const bool marcher = (i < kBufferBindings);
        buffer_infos[i].buffer = marcher ? marcher_buffers[i] : resolve_buffers[i - kBufferBindings];
        buffer_infos[i].offset = 0;
        buffer_infos[i].range = VK_WHOLE_SIZE;
        buffer_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        buffer_writes[i].dstSet = marcher ? descriptor_set_ : resolve_set_;
        // Resolve's storage buffers are bindings 2, 3 and 5 — 4 is the parameter block.
        const u32 resolve_index = i - kBufferBindings;
        buffer_writes[i].dstBinding =
            marcher ? (kImageBindings + i) : ((resolve_index < 2) ? (2 + resolve_index) : 5);
        buffer_writes[i].descriptorCount = 1;
        buffer_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        buffer_writes[i].pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(device_.handle(), kBufferBindings + kResolveBuffers, buffer_writes, 0,
                           nullptr);

    // The test scene spans about 64 m. Start at one corner of it, above the ground slab,
    // looking back toward the origin so the towers, arch and lattice are all in frame.
    camera_.set_position_metres(-22.0, 5.0, -22.0);
    camera_.set_look(45.0, -8.0);
    if (!options_.camera.empty()) {
        f64 values[5]{-22.0, 5.0, -22.0, 45.0, -8.0};
        const char* cursor = options_.camera.c_str();
        for (u32 i = 0; i < 5 && *cursor != '\0'; ++i) {
            values[i] = std::strtod(cursor, const_cast<char**>(&cursor));
            if (*cursor == ',') ++cursor;
        }
        camera_.set_position_metres(values[0], values[1], values[2]);
        camera_.set_look(values[3], values[4]);
    }
    debug_mode_ = options_.debug_mode;

    if (!hud_.create(device_, window_, swapchain_.format())) return 1;

    // The compile time, every run. A stale binary is otherwise invisible: the build tool
    // once reported "no work to do" over a source file that had changed, and a measurement
    // was taken against code that no longer existed. One line makes that impossible to miss.
    WS_LOG_INFO("app", "WorldShaper {}, compiled {} {}", kVersionTag, __DATE__, __TIME__);

    // Sweep up the previous executable an earlier update left behind, then ask GitHub
    // whether there is a newer release. The check is on its own thread and never blocks
    // starting; nothing is downloaded unless the player says so.
    Updater::clean_up_previous();
    if (!options_.no_update_check) updater_.begin_check();
    WS_LOG_INFO("app", "ready. F1 developer panel, F2 overlay, F5 reload shaders, Esc quit");

    const u64 start_ns = now_ns();
    u64 last_ns = start_ns;

    while (window_.pump()) {
        const u64 frame_start = now_ns();
        stats_.push(ns_to_ms(frame_start - last_ns));
        last_ns = frame_start;

        const InputState& input = window_.input();
        if (input.was_pressed(Key::Escape)) {
            if (mouse_look_) {
                mouse_look_ = false;   // first Escape releases the mouse
                window_.set_relative_mouse(false);
            } else {
                break;
            }
        }
        if (input.was_pressed(Key::F1)) hud_.toggle_developer_panel();
        if (input.was_pressed(Key::F2)) hud_.toggle_overlay();
        if (input.was_pressed(Key::F3)) debug_mode_ = (debug_mode_ + 1) % 5;
        if (input.was_pressed(Key::F5)) { visibility_.force_reload(); resolve_.force_reload(); }
        if (input.was_pressed(Key::F11)) swapchain_.set_vsync(!swapchain_.vsync());
        // The only thing that starts a download. Nothing else does, and nothing does it
        // automatically.
        if (input.was_pressed(Key::F8) && updater_.state() == UpdateState::Available) {
            updater_.begin_download();
        }

        // Clicking the world captures the mouse; from then on the buttons belong to the
        // chisel. That first click is swallowed, or capturing the mouse would also start a
        // cut you never asked for.
        if (!mouse_look_) {
            if ((input.mouse_left || input.mouse_right) && !hud_.wants_mouse()) {
                mouse_look_ = true;
                swallow_click_ = true;
                window_.set_relative_mouse(true);
            }
        } else if (swallow_click_ && !input.mouse_left && !input.mouse_right) {
            swallow_click_ = false;
        }

        visibility_.reload_if_changed();
        resolve_.reload_if_changed();

        // Who gets the wheel this frame. It is the most contested input in the game, so the
        // rule is written once, here, rather than being discovered by each tool:
        //   a number key held  -> cycle tools within that slot
        //   G held             -> the chisel's working distance
        //   otherwise          -> the active tool, and the free camera if it does not want it
        u32 held_slot = kToolSlots;
        for (u32 slot = 0; slot < kToolSlots; ++slot) {
            const Key key = static_cast<Key>(static_cast<u16>(Key::Digit1) + slot);
            if (input.is_down(key)) {
                held_slot = slot;
                break;
            }
        }
        const bool cycling = held_slot < kToolSlots;
        const bool chisel_has_wheel = input.is_down(Key::G);
        // The clipboard only claims the wheel once it is holding something. With nothing
        // selected it has nothing to slide, so the wheel goes back to flight speed — which
        // is what you want while flying somewhere to make a selection.
        const bool clipboard_has_wheel = !cycling && !chisel_has_wheel &&
                                         toolbelt_.active() == ToolKind::Clipboard &&
                                         clipboard_.holding();
        const bool tool_has_wheel = cycling || chisel_has_wheel || clipboard_has_wheel;

        const ToolKind tool_before = toolbelt_.active();
        for (u32 slot = 0; slot < kToolSlots; ++slot) {
            const Key key = static_cast<Key>(static_cast<u16>(Key::Digit1) + slot);
            if (input.was_pressed(key)) toolbelt_.select_slot(slot);
        }
        if (cycling && input.wheel != 0.0f) {
            toolbelt_.select_slot(held_slot);
            toolbelt_.cycle(static_cast<i32>(input.wheel));
        }
        // Putting a tool away puts down what it was holding. A ghost that survived a trip
        // through the chisel and reappeared later would be a surprise, not a convenience.
        if (toolbelt_.active() != tool_before) clipboard_.drop();

        const f64 dt = (stats_.last_ms() > 0.0) ? stats_.last_ms() * 0.001 : 1.0 / 60.0;
        camera_.update(input, (dt > 0.1) ? 0.1 : dt, mouse_look_, !tool_has_wheel);
        update_tools(input, chisel_has_wheel, clipboard_has_wheel, (dt > 0.1) ? 0.1 : dt);

        if (window_.minimised()) continue;
        if (window_.resized_this_frame() || swapchain_.needs_recreate()) handle_resize();

        hud_.begin_frame();
        hud_.draw(stats_, profiler_, device_.caps(), swapchain_);

        if (!swapchain_.begin_frame()) {
            // The swapchain is being rebuilt; close the ImGui frame we already opened so
            // the next NewFrame is balanced.
            ImGui::EndFrame();
            continue;
        }

        record_frame(static_cast<f32>(ns_to_ms(frame_start - start_ns) * 0.001));
        swapchain_.end_frame();

        if (!options_.screenshot.empty() && frame_counter_ >= options_.screenshot_frame) {
            device_.wait_idle();
            save_image_png(device_, render_target_, options_.screenshot);
            const std::vector<PassTiming>& passes = profiler_.results();
            for (const PassTiming& pass : passes) {
                WS_LOG_INFO("frame", "{:<12} {:.3f} ms  (budget {:.2f})", pass.name,
                            pass.gpu_ms, pass.budget_ms);
            }
            WS_LOG_INFO("frame", "total GPU {:.3f} ms, CPU {:.3f} ms",
                        profiler_.total_gpu_ms(), stats_.average_ms());
            const ResidencyStats residency = residency_.stats();
            WS_LOG_INFO("frame",
                        "resident {} of {} chunks, {} bricks; feedback {} reports ({} dropped)",
                        residency.resident_chunks, world_.chunk_count(),
                        residency.resident_bricks, last_feedback_,
                        last_feedback_truncated_);
            break;
        }
    }

    device_.wait_idle();
    hud_.destroy();
    world_buffers_.destroy();
    feedback_.destroy();
    destroy_buffer(device_, params_buffer_);
    destroy_buffer(device_, clip_buffer_);
    destroy_buffer(device_, clip_staging_);
    visibility_.destroy();
    resolve_.destroy();
    destroy_render_target();
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_.handle(), descriptor_pool_, nullptr);
    }
    if (set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), set_layout_, nullptr);
    }
    if (resolve_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), resolve_layout_, nullptr);
    }
    profiler_.destroy();
    swapchain_.destroy();
    device_.destroy();
    window_.destroy();
    return 0;
}

}  // namespace
}  // namespace ws

int main(int argc, char** argv) {
    const ws::Options options = ws::parse_options(argc, argv);
    if (options.help) {
        ws::print_help();
        return 0;
    }
    if (options.stream_audit) return ws::run_stream_audit(options);
    if (options.headless) return ws::run_headless(options);

    ws::Application app;
    return app.run(options);
}
