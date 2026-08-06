#include "debug/hud.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>

#include <cstdio>

namespace ws {
namespace {

void hud_event_hook(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
}

const char* format_bytes(u64 bytes, char* buffer, usize size) {
    if (bytes >= (1ull << 30)) {
        std::snprintf(buffer, size, "%.2f GB", static_cast<f64>(bytes) / (1ull << 30));
    } else if (bytes >= (1ull << 20)) {
        std::snprintf(buffer, size, "%.1f MB", static_cast<f64>(bytes) / (1ull << 20));
    } else if (bytes >= (1ull << 10)) {
        std::snprintf(buffer, size, "%.1f KB", static_cast<f64>(bytes) / (1ull << 10));
    } else {
        std::snprintf(buffer, size, "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

}  // namespace

bool Hud::create(Device& device, Window& window, VkFormat colour_format) {
    device_ = &device;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini litter next to the exe
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(window.handle())) {
        WS_LOG_ERROR("hud", "ImGui_ImplSDL3_InitForVulkan failed");
        return false;
    }
    window.set_event_hook(&hud_event_hook);

    VkFormat colour = colour_format;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colour;

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = device_->instance();
    info.PhysicalDevice = device_->physical();
    info.Device = device_->handle();
    info.QueueFamily = device_->graphics_family();
    info.Queue = device_->graphics_queue();
    // Let the backend own its descriptor pool. Its internal descriptor types have changed
    // between ImGui versions, so a pool we size ourselves is a standing source of
    // validation warnings for no benefit.
    info.DescriptorPool = VK_NULL_HANDLE;
    info.DescriptorPoolSize = 64;
    info.MinImageCount = kFramesInFlight;
    info.ImageCount = kFramesInFlight;
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = rendering;

    if (!ImGui_ImplVulkan_Init(&info)) {
        WS_LOG_ERROR("hud", "ImGui_ImplVulkan_Init failed");
        return false;
    }

    initialised_ = true;
    return true;
}

void Hud::destroy() {
    if (!initialised_) return;
    device_->wait_idle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    initialised_ = false;
}

bool Hud::wants_mouse() const {
    return initialised_ && ImGui::GetIO().WantCaptureMouse;
}

void Hud::begin_frame() {
    if (!initialised_) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void Hud::draw(const FrameStats& stats, const GpuProfiler& profiler,
               const DeviceCapabilities& caps, const Swapchain& swapchain) {
    if (!initialised_) return;
    char buffer[64];

    // Reported once, because it is the difference that put the tool readout off the bottom
    // of the screen: the swapchain is in pixels and the interface is in the window's
    // logical units, and on a scaled display those are not the same number.
    if (!reported_scale_) {
        reported_scale_ = true;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (static_cast<u32>(display.x) != swapchain.extent().width ||
            static_cast<u32>(display.y) != swapchain.extent().height) {
            WS_LOG_INFO("hud", "display scaling: interface {}x{}, swapchain {}x{}",
                        static_cast<u32>(display.x), static_cast<u32>(display.y),
                        swapchain.extent().width, swapchain.extent().height);
        }
    }

    if (show_overlay_) {
        // The shipped overlay: small, corner-anchored, no interaction.
        ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin("##overlay", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
        const f64 fps = (stats.last_ms() > 0.0) ? 1000.0 / stats.last_ms() : 0.0;
        ImGui::Text("%.0f FPS   %.2f ms", fps, stats.last_ms());
        ImGui::Text("99th %.2f ms   max %.2f ms", stats.percentile_ms(0.99), stats.max_ms());
        ImGui::Text("GPU  %.2f ms   %s", profiler.total_gpu_ms(),
                    format_bytes(profiler.total_bytes(), buffer, sizeof(buffer)));
        ImGui::Text("%ux%u", swapchain.extent().width, swapchain.extent().height);
        ImGui::End();
    }

    // The chisel's own readout: the size of the cut and how many voxels it is. Shown while
    // a drag is in progress and while the working distance is being set, and at no other
    // time — an always-on counter is furniture (documentation/14 §1).
    if (tool_.active && (tool_.dragging || !tool_.snapping || tool_.holding)) {
        // Positioned from ImGui's own display size, never from the swapchain extent. The
        // swapchain is in pixels and ImGui is in the window's logical units; on a display
        // with scaling those differ by the scale factor, so anchoring the readout to the
        // bottom of the screen in pixels puts it off the bottom of the screen.
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y - 48.0f), ImGuiCond_Always,
                                ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.30f);
        ImGui::Begin("##tool", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoInputs);
        const long long sx = static_cast<long long>(tool_.max[0] - tool_.min[0] + 1);
        const long long sy = static_cast<long long>(tool_.max[1] - tool_.min[1] + 1);
        const long long sz = static_cast<long long>(tool_.max[2] - tool_.min[2] + 1);
        if (tool_.too_large) {
            ImGui::TextColored(ImVec4(0.85f, 0.16f, 0.16f, 1.0f),
                               "%lld x %lld x %lld   too large", sx, sy, sz);
        } else if (tool_.holding) {
            ImGui::Text("clip %lld x %lld x %lld   %d copies %s   %s%s", sx, sy, sz,
                        (tool_.copies < 0) ? -tool_.copies : tool_.copies,
                        (tool_.copies < 0) ? "past the ghost" : "to the ghost",
                        tool_.paste_mode, tool_.grid_snap ? "   snapped" : "");
            ImGui::Text("/ adjusts: %s      scale %s", tool_.adjust_mode, tool_.scale_text);
        } else if (tool_.dragging) {
            ImGui::Text("%lld x %lld x %lld   %llu voxels   %.2f x %.2f x %.2f m", sx, sy, sz,
                        static_cast<unsigned long long>(tool_.volume),
                        static_cast<f64>(sx) / kVoxelsPerMetre,
                        static_cast<f64>(sy) / kVoxelsPerMetre,
                        static_cast<f64>(sz) / kVoxelsPerMetre);
        } else {
            ImGui::Text("reach %.2f m", tool_.distance_metres);
        }
        ImGui::End();
    }

    // The belt, along the bottom. Nine slots, the active one marked, each showing how many
    // tools are stacked on it — the only way to know a number has more than one behind it
    // before the interface stage gives it icons.
    if (show_overlay_) {
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y - 12.0f), ImGuiCond_Always,
                                ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.30f);
        ImGui::Begin("##belt", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoInputs);
        for (u32 slot = 0; slot < 9; ++slot) {
            if (slot > 0) ImGui::SameLine();
            const bool active = slot == tool_.active_slot;
            if (tool_.slot_count[slot] == 0) {
                ImGui::TextDisabled("%u", slot + 1);
            } else if (active) {
                ImGui::Text("[%u %s %u/%u]", slot + 1, tool_.active_tool,
                            tool_.slot_position[slot] + 1, tool_.slot_count[slot]);
            } else {
                ImGui::Text("%u:%u", slot + 1, tool_.slot_count[slot]);
            }
        }
        ImGui::End();
    }

    if (!show_developer_) return;

    ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(12.0f, 130.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("WorldShaper — developer", &show_developer_);

    if (ImGui::CollapsingHeader("Device", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%s", caps.name.c_str());
        ImGui::Text("Vulkan %u.%u.%u", VK_API_VERSION_MAJOR(caps.api_version),
                    VK_API_VERSION_MINOR(caps.api_version),
                    VK_API_VERSION_PATCH(caps.api_version));
        ImGui::Text("device-local memory: %s",
                    format_bytes(caps.device_local_bytes, buffer, sizeof(buffer)));
        ImGui::Text("subgroup size: %u   max workgroup: %u", caps.subgroup_size,
                    caps.max_workgroup_invocations);
        ImGui::Text("ray query: %s   int64 atomics: %s", caps.has_ray_query ? "yes" : "no",
                    caps.has_int64_atomics ? "yes" : "no");
    }

    if (ImGui::CollapsingHeader("Frame", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("frame %llu", static_cast<unsigned long long>(stats.frame_index()));
        ImGui::Text("cpu   last %.3f ms   avg %.3f ms", stats.last_ms(), stats.average_ms());
        ImGui::Text("      50th %.3f   99th %.3f   max %.3f", stats.percentile_ms(0.50),
                    stats.percentile_ms(0.99), stats.max_ms());
        ImGui::Text("present: %s", swapchain.vsync() ? "vsync (FIFO)" : "uncapped");
    }

    if (ImGui::CollapsingHeader("GPU passes", ImGuiTreeNodeFlags_DefaultOpen)) {
        // documentation/09: every pass has a time budget and a bandwidth budget. Over
        // budget is a bug, so it is coloured like one.
        if (ImGui::BeginTable("passes", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("pass");
            ImGui::TableSetupColumn("ms");
            ImGui::TableSetupColumn("bytes");
            ImGui::TableHeadersRow();
            for (const PassTiming& pass : profiler.results()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(pass.name.c_str());
                ImGui::TableNextColumn();
                const bool over = pass.budget_ms > 0.0 && pass.gpu_ms > pass.budget_ms;
                if (over) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                if (pass.budget_ms > 0.0) {
                    ImGui::Text("%.3f / %.2f", pass.gpu_ms, pass.budget_ms);
                } else {
                    ImGui::Text("%.3f", pass.gpu_ms);
                }
                if (over) ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(format_bytes(pass.bytes, buffer, sizeof(buffer)));
            }
            ImGui::EndTable();
        }
        ImGui::Text("total GPU %.3f ms   %s", profiler.total_gpu_ms(),
                    format_bytes(profiler.total_bytes(), buffer, sizeof(buffer)));
    }

    if (ImGui::CollapsingHeader("Streaming", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("chunks: %llu resident of %llu in world",
                    static_cast<unsigned long long>(streaming_.resident_chunks),
                    static_cast<unsigned long long>(streaming_.world_chunks));
        ImGui::Text("bricks resident: %llu   evictions: %llu",
                    static_cast<unsigned long long>(streaming_.resident_bricks),
                    static_cast<unsigned long long>(streaming_.evictions));
        // Two buffers: both arguments are evaluated before the call, so sharing one would
        // print the same number twice.
        char capacity_text[64];
        ImGui::Text("payload: %s of %s",
                    format_bytes(streaming_.payload_in_use, buffer, sizeof(buffer)),
                    format_bytes(streaming_.payload_capacity, capacity_text,
                                 sizeof(capacity_text)));
        ImGui::Text("resident total: %s",
                    format_bytes(streaming_.resident_bytes, buffer, sizeof(buffer)));
        ImGui::Text("staged this frame: %s in %u copies (%u before merging)",
                    format_bytes(streaming_.staged_bytes, buffer, sizeof(buffer)),
                    streaming_.copy_regions, streaming_.raw_regions);
        ImGui::Text("cache hit rate: %.1f%%", streaming_.hit_rate * 100.0);

        // The CPU side of streaming, which is what the 0.8 ms budget is actually about.
        const bool over = streaming_.update_ms > 0.8;
        if (over) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::Text("residency update (CPU): %.3f / 0.80 ms   worst %.3f ms",
                    streaming_.update_ms, streaming_.worst_update_ms);
        if (over) ImGui::PopStyleColor();

        ImGui::Text("feedback: %llu reports%s",
                    static_cast<unsigned long long>(streaming_.feedback_reports),
                    (streaming_.feedback_dropped > 0) ? " (buffer full)" : "");
        if (streaming_.deferred_bytes > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "deferred: %s",
                               format_bytes(streaming_.deferred_bytes, buffer, sizeof(buffer)));
        }
        if (streaming_.out_of_memory) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "OUT OF POOL - raise the residency budget");
        }
    }

    if (ImGui::CollapsingHeader("Tools", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("active: %s (slot %u)", tool_.active_tool, tool_.active_slot + 1);
        ImGui::TextDisabled("tap 1-9 to pick a slot, hold one and scroll to cycle it");
        if (tool_.holding) {
            ImGui::Text("clip: %d copies %s   %s", (tool_.copies < 0) ? -tool_.copies : tool_.copies,
                        (tool_.copies < 0) ? "beyond the ghost" : "up to the ghost", tool_.paste_mode);
            ImGui::Text("grid snap: %s   turn step %.2f deg",
                        tool_.grid_snap ? "on (copies tile)" : "off", tool_.turn_step);
            ImGui::Text("angles: %.2f, %.2f, %.2f deg", tool_.angles[0], tool_.angles[1],
                        tool_.angles[2]);
        }
        ImGui::Text("mode: %s", tool_.dragging ? (tool_.carving ? "carving" : "placing")
                                               : "idle");
        ImGui::Text("P overwrite: %s   O first point: %s",
                    tool_.overwrite ? "replaces what is there" : "fills empty space only",
                    tool_.against_face ? "against the face" : "the voxel you look at");
        if (tool_.snapping) {
            ImGui::TextUnformatted("distance: snapped to the aimed voxel");
        } else {
            ImGui::Text("distance: %.2f m", tool_.distance_metres);
        }
        if (tool_.active) {
            ImGui::Text("box: %lld,%lld,%lld to %lld,%lld,%lld",
                        static_cast<long long>(tool_.min[0]), static_cast<long long>(tool_.min[1]),
                        static_cast<long long>(tool_.min[2]), static_cast<long long>(tool_.max[0]),
                        static_cast<long long>(tool_.max[1]),
                        static_cast<long long>(tool_.max[2]));
            ImGui::Text("volume: %llu voxels%s",
                        static_cast<unsigned long long>(tool_.volume),
                        tool_.too_large ? "  (over the per-edit limit)" : "");
        }
        ImGui::Text("constraint points: %u", tool_.constraints);
        ImGui::Text("last edit: %llu voxels in %.3f ms",
                    static_cast<unsigned long long>(tool_.last_edit_voxels), tool_.last_edit_ms);
        ImGui::Text("undo %llu   redo %llu   history %s",
                    static_cast<unsigned long long>(tool_.undo_depth),
                    static_cast<unsigned long long>(tool_.redo_depth),
                    format_bytes(tool_.history_bytes, buffer, sizeof(buffer)));
        ImGui::Text("ops logged: %llu", static_cast<unsigned long long>(tool_.ops_logged));
    }

    ImGui::Separator();
    ImGui::TextDisabled("F1 developer panel   F2 overlay   F5 reload shaders   F11 vsync");
    ImGui::TextDisabled("move:      WASD   space up   C down   shift faster   wheel speed");
    ImGui::TextDisabled("chisel:    LMB carve   RMB place   G+wheel distance   MMB constraint");
    ImGui::TextDisabled("           P overwrite   O first point   Q/E material");
    ImGui::TextDisabled("clipboard: LMB select then LMB stamp   RMB drop   MMB send to crosshair");
    ImGui::TextDisabled("           wheel move (speeds up)   shift+wheel a whole length");
    ImGui::TextDisabled("           arrows turn   / copies or size   O quarter turns   P paste mode");
    ImGui::TextDisabled("           in size: . , resize all   arrows stretch the axis you face");
    ImGui::TextDisabled("both:      Z undo   X redo   R clear/drop   backspace cancel   1-9 tool");
    ImGui::End();
}

void Hud::render(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
    if (!initialised_) return;
    ImGui::Render();

    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = target;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colour;

    vkCmdBeginRendering(cmd, &rendering);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

}  // namespace ws
