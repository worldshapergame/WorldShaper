# Third-party dependencies.
#
# Rules (documentation/08-tech-stack-and-licensing.md):
#   - permissive licences only: MIT / BSD / Apache-2.0 / zlib / ISC / CC0 / public domain
#   - every dependency pinned to an exact tag so nothing can move under us
#   - anything with its own CMake is kept to a minimum; small libraries are compiled
#     directly by us, which removes a whole class of build breakage
#
# Licences: SDL3 = zlib | volk = MIT | VulkanMemoryAllocator = MIT
#           Dear ImGui = MIT | doctest = MIT

include(FetchContent)

# Several upstream projects still declare cmake_minimum_required(VERSION 3.x < 3.5),
# which CMake 4 rejects outright. This lets them configure without patching them.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

set(FETCHCONTENT_QUIET OFF)

# --------------------------------------------------------------------------------------
# Vulkan headers + shader compiler (from the installed SDK; we never link the loader,
# volk resolves entry points at runtime)
# --------------------------------------------------------------------------------------
find_package(Vulkan REQUIRED COMPONENTS glslc)

# --------------------------------------------------------------------------------------
# SDL3 — window, input, gamepad, timing
# --------------------------------------------------------------------------------------
set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
set(SDL_TESTS         OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL       OFF CACHE BOOL "" FORCE)
set(SDL_SHARED        ON  CACHE BOOL "" FORCE)
set(SDL_STATIC        OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.14
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(SDL3)

# --------------------------------------------------------------------------------------
# volk — Vulkan meta-loader (two files; we compile them ourselves)
# --------------------------------------------------------------------------------------
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        vulkan-sdk-1.4.341.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-here)
FetchContent_MakeAvailable(volk)

add_library(ws_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(ws_volk PUBLIC "${volk_SOURCE_DIR}")
target_link_libraries(ws_volk PUBLIC Vulkan::Headers)
target_compile_definitions(ws_volk PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(ws_volk PUBLIC VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --------------------------------------------------------------------------------------
# VulkanMemoryAllocator — single header, compiled into one translation unit by us
# --------------------------------------------------------------------------------------
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-here)
FetchContent_MakeAvailable(vma)

add_library(ws_vma INTERFACE)
target_include_directories(ws_vma INTERFACE "${vma_SOURCE_DIR}/include")
target_link_libraries(ws_vma INTERFACE ws_volk)

# --------------------------------------------------------------------------------------
# Dear ImGui — developer HUD only; the game's own UI is built from scratch and diegetic
# --------------------------------------------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-here)
FetchContent_MakeAvailable(imgui)

add_library(ws_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(ws_imgui PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends")
target_link_libraries(ws_imgui PUBLIC SDL3::SDL3 ws_volk)
# Route ImGui's Vulkan backend through volk instead of the static loader.
target_compile_definitions(ws_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)

# --------------------------------------------------------------------------------------
# stb — public domain single-header image writing, for screenshots and for the automated
# render checks that compare a rendered frame against a reference
# --------------------------------------------------------------------------------------
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        f58f558c120e9b32c217290b80bad1a0729fbb2c
    SOURCE_SUBDIR  no-cmake-here)
FetchContent_MakeAvailable(stb)

add_library(ws_stb INTERFACE)
target_include_directories(ws_stb INTERFACE "${stb_SOURCE_DIR}")

# --------------------------------------------------------------------------------------
# doctest — unit tests
# --------------------------------------------------------------------------------------
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.5.3
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-here)
FetchContent_MakeAvailable(doctest)

add_library(ws_doctest INTERFACE)
target_include_directories(ws_doctest INTERFACE "${doctest_SOURCE_DIR}")
