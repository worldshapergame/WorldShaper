cmake_minimum_required(VERSION 3.24)
project(WorldShaper LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(WS_SANITIZE "Enable address/UB sanitizers in debug builds" OFF)

include(cmake/Dependencies.cmake)

# --------------------------------------------------------------------------------------
# Compile options shared by all of our own targets.
#
# We have no second engineer reviewing this code (documentation/08 section 1), so warnings
# are errors and the sanitizers are one flag away.
# --------------------------------------------------------------------------------------
add_library(ws_options INTERFACE)
if(MSVC)
    target_compile_options(ws_options INTERFACE
        /W4 /WX /permissive- /Zc:preprocessor /Zc:__cplusplus /utf-8 /MP
        /wd4324   # structure padded due to alignas - intentional throughout
    )
    target_compile_definitions(ws_options INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ws_options INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

if(WS_SANITIZE AND MSVC)
    target_compile_options(ws_options INTERFACE /fsanitize=address)
endif()

target_compile_definitions(ws_options INTERFACE
    $<$<CONFIG:Debug>:WS_DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:WS_DEBUG=0>)

# --------------------------------------------------------------------------------------
# ws_core - allocators, jobs, fixed-point math, logging, hashing.
# Deliberately knows nothing about graphics, windowing, or the game.
# --------------------------------------------------------------------------------------
add_library(ws_core STATIC
    src/core/log.cpp
    src/core/jobs.cpp
    src/core/time.cpp
    src/core/block_pool.cpp)
target_include_directories(ws_core PUBLIC src)
target_link_libraries(ws_core PUBLIC ws_options)

# --------------------------------------------------------------------------------------
# ws_world - the voxel data model: bricks, chunks, voxel types, tags, properties, ops,
# the matter ledger and serialisation.
#
# Knows nothing about rendering, networking, the GPU, or gameplay. It is a data structure
# with an edit API and an invariant checker, and it runs headless in tests.
# --------------------------------------------------------------------------------------
add_library(ws_world STATIC
    src/world/tags.cpp
    src/world/property.cpp
    src/world/voxel_type.cpp
    src/world/brick.cpp
    src/world/chunk.cpp
    src/world/world.cpp
    src/world/ledger.cpp
    src/world/op.cpp
    src/world/raycast.cpp
    src/world/region.cpp
    src/world/history.cpp
    src/world/serialize.cpp
    src/world/gpu_brick.cpp
    src/world/residency.cpp
    src/world/test_scene.cpp)
target_include_directories(ws_world PUBLIC src)
target_link_libraries(ws_world PUBLIC ws_core)

# --------------------------------------------------------------------------------------
# ws_game - tools and gameplay that act on the world. Deliberately free of any dependency
# on the window or the GPU, so it can be tested headless and driven by the network later.
# --------------------------------------------------------------------------------------
add_library(ws_game STATIC
    src/game/chisel.cpp
    src/game/clip.cpp
    src/game/clipboard.cpp
    src/game/toolbelt.cpp)
target_include_directories(ws_game PUBLIC src)
target_link_libraries(ws_game PUBLIC ws_core ws_world)

# --------------------------------------------------------------------------------------
# ws_platform - window, input, event pump.
# --------------------------------------------------------------------------------------
add_library(ws_platform STATIC
    src/platform/window.cpp)
target_include_directories(ws_platform PUBLIC src)
target_link_libraries(ws_platform PUBLIC ws_core SDL3::SDL3)

# --------------------------------------------------------------------------------------
# ws_gpu - Vulkan device, swapchain, per-frame ring, shader loading and hot reload.
# --------------------------------------------------------------------------------------
add_library(ws_gpu STATIC
    src/gpu/device.cpp
    src/gpu/swapchain.cpp
    src/gpu/shader.cpp
    src/gpu/profiler.cpp
    src/gpu/vma_impl.cpp
    src/gpu/world_buffers.cpp
    src/gpu/screenshot.cpp
    src/gpu/feedback.cpp)
target_include_directories(ws_gpu PUBLIC src)
target_link_libraries(ws_gpu PUBLIC ws_core ws_world ws_platform ws_volk ws_vma ws_stb)

# VMA is a third-party single-header implementation; do not hold it to our warning bar.
if(MSVC)
    set_source_files_properties(src/gpu/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(src/gpu/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

# --------------------------------------------------------------------------------------
# WorldShaper - the game executable.
# --------------------------------------------------------------------------------------
add_executable(WorldShaper
    src/app/main.cpp
    src/debug/hud.cpp)
target_link_libraries(WorldShaper PRIVATE ws_core ws_world ws_game ws_platform ws_gpu ws_imgui)

# Where the shader *sources* live, so hot reload works regardless of the working
# directory. Shipped builds fall back to the .spv files next to the executable.
target_compile_definitions(WorldShaper PRIVATE
    WS_SHADER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    WS_EXE_SHADER_DIR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")

if(WIN32)
    add_custom_command(TARGET WorldShaper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:WorldShaper>)
endif()

# --------------------------------------------------------------------------------------
# Shaders - compiled to SPIR-V next to the executable, and recompilable at runtime by the
# hot-reload path (which shells out to the same glslc).
# --------------------------------------------------------------------------------------
set(WS_SHADER_OUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
file(GLOB WS_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp"
                     "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert"
                     "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag")
set(WS_SPV_FILES "")
foreach(shader ${WS_SHADERS})
    get_filename_component(name "${shader}" NAME)
    set(spv "${WS_SHADER_OUT}/${name}.spv")
    add_custom_command(
        OUTPUT  "${spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WS_SHADER_OUT}"
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -g -O "${shader}" -o "${spv}"
        DEPENDS "${shader}"
        COMMENT "glslc ${name}"
        VERBATIM)
    list(APPEND WS_SPV_FILES "${spv}")
endforeach()
add_custom_target(ws_shaders ALL DEPENDS ${WS_SPV_FILES})
add_dependencies(WorldShaper ws_shaders)

# Record where glslc lives so runtime hot reload can find it without a PATH lookup.
target_compile_definitions(ws_gpu PUBLIC WS_GLSLC_PATH="${Vulkan_GLSLC_EXECUTABLE}")

# --------------------------------------------------------------------------------------
# Tests - these are the safety net, not an afterthought.
# --------------------------------------------------------------------------------------
enable_testing()
add_executable(ws_tests
    tests/test_main.cpp
    tests/test_fixed.cpp
    tests/test_hash.cpp
    tests/test_arena.cpp
    tests/test_ring.cpp
    tests/test_jobs.cpp
    tests/test_tags.cpp
    tests/test_property.cpp
    tests/test_voxel_type.cpp
    tests/test_brick.cpp
    tests/test_chunk.cpp
    tests/test_world.cpp
    tests/test_op.cpp
    tests/test_raycast.cpp
    tests/test_history.cpp
    tests/test_chisel.cpp
    tests/test_clip.cpp
    tests/test_clipboard.cpp
    tests/test_toolbelt.cpp
    tests/test_serialize.cpp
    tests/test_residency.cpp)
target_link_libraries(ws_tests PRIVATE ws_core ws_world ws_game ws_doctest)
add_test(NAME unit COMMAND ws_tests)
