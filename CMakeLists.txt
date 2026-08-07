cmake_minimum_required(VERSION 3.24)

# The one place the version number lives.
#
# Everything else reads it from here: the window title, the developer panel, the release
# workflow's tag, the name of the zip, and the check the game makes against the latest
# release to decide whether it is out of date. Bump it here, tag `v<the same number>`, and
# the release that comes out matches the build that announces itself.
project(WorldShaper VERSION 0.6.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(WS_SANITIZE "Enable address/UB sanitizers in debug builds" OFF)

# The version, and the Windows resources that carry it, generated from one source.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.hpp.in"
               "${CMAKE_BINARY_DIR}/generated/core/version.hpp" @ONLY)
add_library(ws_version INTERFACE)
target_include_directories(ws_version INTERFACE "${CMAKE_BINARY_DIR}/generated")

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
    src/core/crash.cpp
    src/core/jobs.cpp
    src/core/time.cpp
    src/core/block_pool.cpp)
target_include_directories(ws_core PUBLIC src)
target_link_libraries(ws_core PUBLIC ws_options ws_version)
if(WIN32)
    # dbghelp walks the stack and writes the minidump; user32 shows the dialog that tells a
    # player a report exists. Both ship with Windows, so neither adds a redistributable.
    target_link_libraries(ws_core PUBLIC dbghelp user32)
endif()

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
    src/world/world_cache.cpp
    src/world/gpu_brick.cpp
    src/world/residency.cpp
    src/world/light_list.cpp
    src/world/thumbnail.cpp
    src/world/thumb_cache.cpp
    src/world/summary_tree.cpp)
target_include_directories(ws_world PUBLIC src)
target_link_libraries(ws_world PUBLIC ws_core)

# --------------------------------------------------------------------------------------
# ws_game - tools and gameplay that act on the world. Deliberately free of any dependency
# on the window or the GPU, so it can be tested headless and driven by the network later.
# --------------------------------------------------------------------------------------
add_library(ws_game STATIC
    src/game/quality.cpp
    src/game/chisel.cpp
    src/game/clip.cpp
    src/game/clipboard.cpp
    src/game/toolbelt.cpp)
target_include_directories(ws_game PUBLIC src)
target_link_libraries(ws_game PUBLIC ws_core ws_world)

# --------------------------------------------------------------------------------------
# ws_forge - authoring clips procedurally: the field language every shape and pattern is
# made of, the sampler that turns one into voxels, and the measurements that check it is
# what was intended. Headless and free of the GPU, so a clip can be built and reported on
# from a test.
# --------------------------------------------------------------------------------------
add_library(ws_forge STATIC
    src/forge/field.cpp
    src/forge/sample.cpp
    src/forge/measure.cpp
    src/forge/clip_script.cpp)
target_include_directories(ws_forge PUBLIC src)
target_link_libraries(ws_forge PUBLIC ws_core ws_world ws_game)

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
set(WS_APP_SOURCES
    src/app/main.cpp
    src/app/updater.cpp
    src/debug/hud.cpp)

# The icon and the version block. Windows only — on other platforms the icon is the .png in
# assets/ and the desktop environment picks it up from a .desktop file instead.
if(WIN32)
    set(WS_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
    if(EXISTS "${WS_ICON_PATH}")
        # Backslashes, because the resource compiler reads the path as a C string and a
        # forward slash in a Windows path is an escape waiting to happen.
        file(TO_NATIVE_PATH "${WS_ICON_PATH}" WS_ICON_PATH)
        string(REPLACE "\\" "\\\\" WS_ICON_PATH "${WS_ICON_PATH}")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/app/WorldShaper.rc.in"
                       "${CMAKE_BINARY_DIR}/generated/WorldShaper.rc" @ONLY)
        list(APPEND WS_APP_SOURCES "${CMAKE_BINARY_DIR}/generated/WorldShaper.rc")
    else()
        message(WARNING "assets/icon.ico is missing; the executable will have no icon. "
                        "Run tools/make-icon.ps1 to build it from assets/icon.png.")
    endif()
endif()

add_executable(WorldShaper ${WS_APP_SOURCES})
target_link_libraries(WorldShaper PRIVATE
    ws_core ws_world ws_game ws_forge ws_platform ws_gpu ws_imgui ws_version)
if(WIN32)
    # The update check speaks HTTPS through the system's own stack, so there is no HTTP
    # library to depend on, to license, or to keep patched.
    target_link_libraries(WorldShaper PRIVATE winhttp)
endif()

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
# Shared GLSL, included rather than compiled on its own. Every .comp depends on all of it:
# editing world.glsl has to rebuild the shaders that include it, or the next run is quietly
# the old traversal.
file(GLOB WS_SHADER_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl")

set(WS_SPV_FILES "")
foreach(shader ${WS_SHADERS})
    get_filename_component(name "${shader}" NAME)
    set(spv "${WS_SHADER_OUT}/${name}.spv")
    add_custom_command(
        OUTPUT  "${spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WS_SHADER_OUT}"
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -g -O
                -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${shader}" -o "${spv}"
        DEPENDS "${shader}" ${WS_SHADER_INCLUDES}
        COMMENT "glslc ${name}"
        VERBATIM)
    list(APPEND WS_SPV_FILES "${spv}")
endforeach()
add_custom_target(ws_shaders ALL DEPENDS ${WS_SPV_FILES})
add_dependencies(WorldShaper ws_shaders)

# The clips are content, and the facility is the scene now, so they have to travel with the
# executable the way the compiled shaders do.
add_custom_command(TARGET WorldShaper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/clips" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/clips")

# Record where glslc lives so runtime hot reload can find it without a PATH lookup.
target_compile_definitions(ws_gpu PUBLIC WS_GLSLC_PATH="${Vulkan_GLSLC_EXECUTABLE}")

# --------------------------------------------------------------------------------------
# Tests - these are the safety net, not an afterthought.
# --------------------------------------------------------------------------------------
enable_testing()
add_executable(ws_tests
    tests/test_main.cpp
    tests/test_field.cpp
    tests/test_clip_script.cpp
    tests/test_weathering.cpp
    tests/test_sample.cpp
    tests/test_fixed.cpp
    tests/test_hash.cpp
    tests/test_arena.cpp
    tests/test_block_pool.cpp
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
    tests/test_residency.cpp
    tests/test_thumbnail.cpp
    tests/test_thumb_cache.cpp
    tests/test_summary_tree.cpp
    tests/test_hollow.cpp
    tests/test_log_ring.cpp
    tests/test_quality.cpp
    tests/test_light_list.cpp)
target_link_libraries(ws_tests PRIVATE ws_core ws_world ws_game ws_forge ws_doctest)
add_test(NAME unit COMMAND ws_tests)
