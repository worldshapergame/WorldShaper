#include "gpu/shader.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace ws {
namespace {

// Path recorded by CMake so runtime recompiles use exactly the compiler the build used.
constexpr const char* kGlslc =
#ifdef WS_GLSLC_PATH
    WS_GLSLC_PATH;
#else
    "glslc";
#endif

}  // namespace

std::vector<u32> read_spirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        WS_LOG_ERROR("shader", "cannot open {}", path.string());
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % 4) != 0) {
        WS_LOG_ERROR("shader", "{} is not valid SPIR-V ({} bytes)", path.string(), size);
        return {};
    }
    std::vector<u32> words(static_cast<usize>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), size);
    return words;
}

VkShaderModule create_shader_module(VkDevice device, const std::vector<u32>& spirv) {
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spirv.size() * sizeof(u32);
    info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

ComputePipeline::~ComputePipeline() { destroy(); }

bool ComputePipeline::create(Device& device, const std::filesystem::path& source_path,
                             const std::filesystem::path& spirv_path,
                             VkDescriptorSetLayout set_layout, u32 push_constant_bytes,
                             VkDescriptorSetLayout second_set_layout) {
    device_ = &device;
    source_path_ = source_path;
    spirv_path_ = spirv_path;
    set_layout_ = set_layout;
    second_set_layout_ = second_set_layout;
    push_constant_bytes_ = push_constant_bytes;

    std::error_code ec;
    source_time_ = std::filesystem::last_write_time(source_path_, ec);

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = push_constant_bytes_;

    // R12c: a second set, and it is optional so no existing caller changes. The marcher's own
    // buffers fill set 0 -- 0 and 1 are the visibility pass's images and 2..25 the node pool's --
    // so the field the marcher derives from has nowhere to go but set 1. See the remap at the top
    // of shaders/node.glsl.
    const VkDescriptorSetLayout sets[2] = {set_layout_, second_set_layout_};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = (set_layout_ == VK_NULL_HANDLE) ? 0u
                                 : (second_set_layout_ == VK_NULL_HANDLE) ? 1u
                                                                          : 2u;
    layout_info.pSetLayouts = sets;
    layout_info.pushConstantRangeCount = (push_constant_bytes_ > 0) ? 1u : 0u;
    layout_info.pPushConstantRanges = &range;
    WS_VK(vkCreatePipelineLayout(device_->handle(), &layout_info, nullptr, &layout_));

    return build();
}

bool ComputePipeline::build() {
    const std::vector<u32> spirv = read_spirv(spirv_path_);
    if (spirv.empty()) {
        last_error_ = "failed to read " + spirv_path_.string();
        return false;
    }

    VkShaderModule module = create_shader_module(device_->handle(), spirv);
    if (module == VK_NULL_HANDLE) {
        last_error_ = "vkCreateShaderModule failed";
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = stage;
    info.layout = layout_;

    VkPipeline built = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(device_->handle(), VK_NULL_HANDLE, 1,
                                                     &info, nullptr, &built);
    vkDestroyShaderModule(device_->handle(), module, nullptr);

    if (result != VK_SUCCESS) {
        last_error_ = std::string("vkCreateComputePipelines: ") + vk_result_name(result);
        return false;
    }

    // Only replace the working pipeline once the new one exists.
    if (pipeline_ != VK_NULL_HANDLE) {
        device_->wait_idle();
        vkDestroyPipeline(device_->handle(), pipeline_, nullptr);
    }
    pipeline_ = built;
    last_error_.clear();
    return true;
}

bool ComputePipeline::reload_if_changed() {
    if (device_ == nullptr) return false;

    std::error_code ec;
    const auto current = std::filesystem::last_write_time(source_path_, ec);
    if (ec || current == source_time_) return false;
    source_time_ = current;
    return force_reload();
}

bool ComputePipeline::force_reload() {
    if (device_ == nullptr) return false;

    // Shell out to the same glslc the build used. The outer pair of quotes is required by
    // cmd.exe when the executable path itself is quoted.
    std::string command;
    command.reserve(512);
    command += '"';
    command += '"';
    command += kGlslc;
    command += '"';
    command += " --target-env=vulkan1.3 -g -O \"";
    command += source_path_.string();
    command += "\" -o \"";
    command += spirv_path_.string();
    command += "\" 2>&1";
    command += '"';

    const int exit_code = std::system(command.c_str());
    if (exit_code != 0) {
        last_error_ = "shader compile failed; keeping the previous version";
        WS_LOG_WARN("shader", "{} failed to compile (exit {}), keeping previous version",
                    source_path_.filename().string(), exit_code);
        return false;
    }

    if (build()) {
        ++reload_count_;
        WS_LOG_INFO("shader", "reloaded {}", source_path_.filename().string());
        return true;
    }
    WS_LOG_WARN("shader", "{}: {}", source_path_.filename().string(), last_error_);
    return false;
}

void ComputePipeline::destroy() {
    if (device_ == nullptr) return;
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_->handle(), pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_->handle(), layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    device_ = nullptr;
}

}  // namespace ws
