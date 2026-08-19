#pragma once
// Shader loading and hot reload.
//
// Hot reload is not a luxury on this project. The renderer is almost entirely compute
// shaders, and a path tracer is tuned by looking at it. Saving a .comp file and seeing
// the change without restarting is the difference between minutes and seconds per
// iteration, thousands of times over.

#include <filesystem>
#include <string>
#include <vector>

#include "gpu/device.hpp"

namespace ws {

std::vector<u32> read_spirv(const std::filesystem::path& path);
VkShaderModule create_shader_module(VkDevice device, const std::vector<u32>& spirv);

class ComputePipeline {
public:
    ComputePipeline() = default;
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    // source_path is the .comp file; spirv_path is where the build wrote its .spv.
    // Both are kept so the shader can be recompiled in place at runtime.
    // `second_set_layout` is R12c's: the marcher derives from the field, set 0 has no room for it,
    // and every other pipeline passes nothing and is unchanged.
    bool create(Device& device, const std::filesystem::path& source_path,
                const std::filesystem::path& spirv_path,
                VkDescriptorSetLayout set_layout, u32 push_constant_bytes,
                VkDescriptorSetLayout second_set_layout = VK_NULL_HANDLE);
    void destroy();

    // Recompiles and rebuilds only if the source file changed on disk. A compile error
    // logs the compiler output and keeps the previous working pipeline — a typo must
    // never take the game down mid-session.
    bool reload_if_changed();

    // Recompiles unconditionally (bound to F5). Useful when an included file changed,
    // which the timestamp check on the top-level source cannot see.
    bool force_reload();

    VkPipeline pipeline() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }
    u64 reload_count() const { return reload_count_; }
    const std::string& last_error() const { return last_error_; }

private:
    bool build();

    Device* device_ = nullptr;
    std::filesystem::path source_path_;
    std::filesystem::path spirv_path_;
    std::filesystem::file_time_type source_time_{};
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout second_set_layout_ = VK_NULL_HANDLE;
    u32 push_constant_bytes_ = 0;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    u64 reload_count_ = 0;
    std::string last_error_;
};

}  // namespace ws
