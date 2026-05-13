# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# neon-robot-cef — ovrtx + CEF + Vulkan composite.
#
# Pipeline (one binary, one window):
#   ovrtx  ──▶  CUDA  ──▶  Vulkan sampled image  ──┐
#                                                  ├──▶  fullscreen composite ──▶ swapchain
#   CEF OSR  ──▶  CPU bitmap  ──▶  Vulkan sampled image ─┘
#
# This is "cef-bim-test Phase 4" delivered as a sibling project:
# the scene comes from ovrtx (CUDA-interop path, same as neon-robot-c)
# and the UI overlay is a Chromium OSR bitmap composited via the same
# fullscreen.frag (scene_index + ui_index push constants).
#
# Build prerequisites:
#   Windows: Visual Studio 2022/2026 + Vulkan SDK + CUDA Toolkit 12+
#   Linux:   gcc/clang + Vulkan SDK + CUDA Toolkit 12+
#            apt: libgtk-3-dev libnss3 libxss1 libasound2 libgbm1

cmake_minimum_required(VERSION 3.21)
project(neon-robot-cef LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── CRT choice (Windows only) ───────────────────────────────────────
# ovrtx's prebuilt Windows package is built against /MD (dynamic CRT).
# CEF 147 ships both /MT and /MD wrapper variants — pick /MD via the
# flag CEF's cmake exposes. Mixing /MT and /MD causes unresolved
# __imp_strtok / __imp_fmaxf at link time, so this has to be set
# BEFORE find_package(CEF).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "" FORCE)
endif()

# ─── ovrtx (fetched via the shared cmake helper) ─────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake")
include(ovrtx)
ovrtx_fetch()

# ─── Vulkan + glslc ──────────────────────────────────────────────────
find_package(Vulkan REQUIRED)
find_program(GLSLC_EXECUTABLE glslc HINTS $ENV{VULKAN_1_2_SDK_BIN})
if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found. Install the Vulkan SDK.")
endif()
message(STATUS "Found glslc: ${GLSLC_EXECUTABLE}")

# ─── CUDA Toolkit (driver API only) ──────────────────────────────────
find_package(CUDAToolkit REQUIRED)

# ─── Shared FetchContent base with sibling examples ──────────────────
include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(FETCHCONTENT_BASE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../_deps" CACHE PATH "")

# ─── GLFW ────────────────────────────────────────────────────────────
find_package(glfw3 QUIET)
if(NOT glfw3_FOUND)
    FetchContent_Declare(glfw3
        URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw3)
endif()

FetchContent_Declare(unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG v4.4.0)
FetchContent_MakeAvailable(unordered_dense)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.3)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG 1.4.304)
FetchContent_MakeAvailable(volk)

# ─── CEF binary distribution ─────────────────────────────────────────
set(CEF_VERSION "147.0.10+gd58e84d+chromium-147.0.7727.118")
if(WIN32)
    set(CEF_PLATFORM "windows64")
elseif(UNIX AND NOT APPLE)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(CEF_PLATFORM "linuxarm64")
    else()
        set(CEF_PLATFORM "linux64")
    endif()
else()
    message(FATAL_ERROR "Unsupported platform for CEF: ${CMAKE_SYSTEM_NAME}")
endif()
message(STATUS "CEF platform: ${CEF_PLATFORM}")

string(REPLACE "+" "%2B" CEF_VERSION_URL "${CEF_VERSION}")
set(CEF_DIR     "${CMAKE_BINARY_DIR}/cef_binary_${CEF_VERSION}_${CEF_PLATFORM}")
set(CEF_TARBALL "${CMAKE_BINARY_DIR}/cef.tar.bz2")
set(CEF_URL     "https://cef-builds.spotifycdn.com/cef_binary_${CEF_VERSION_URL}_${CEF_PLATFORM}.tar.bz2")

if(NOT EXISTS "${CEF_DIR}/cmake/cef_variables.cmake")
    if(NOT EXISTS "${CEF_TARBALL}")
        message(STATUS "Downloading CEF ${CEF_VERSION} for ${CEF_PLATFORM} (~150-300 MB)...")
        file(DOWNLOAD "${CEF_URL}" "${CEF_TARBALL}" SHOW_PROGRESS STATUS dl_status)
        list(GET dl_status 0 dl_code)
        if(NOT dl_code EQUAL 0)
            message(FATAL_ERROR "CEF download failed: ${dl_status}")
        endif()
    endif()
    message(STATUS "Extracting CEF...")
    file(ARCHIVE_EXTRACT INPUT "${CEF_TARBALL}" DESTINATION "${CMAKE_BINARY_DIR}")
endif()

set(CEF_ROOT "${CEF_DIR}")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
add_subdirectory("${CEF_ROOT}/libcef_dll" libcef_dll_wrapper EXCLUDE_FROM_ALL)

# ─── Compile shaders to SPIR-V ───────────────────────────────────────
set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

set(SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.frag")
set(COMPILED_SHADERS)
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SHADER_OUT "${SHADER_OUT_DIR}/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT  "${SHADER_OUT}"
        COMMAND ${GLSLC_EXECUTABLE} -O --target-env=vulkan1.2
                --target-spv=spv1.5 "${SHADER}" -o "${SHADER_OUT}"
        DEPENDS "${SHADER}"
        COMMENT "Compiling ${SHADER_NAME} -> SPIR-V"
        VERBATIM)
    list(APPEND COMPILED_SHADERS "${SHADER_OUT}")
endforeach()
add_custom_target(neon_robot_cef_shaders ALL DEPENDS ${COMPILED_SHADERS})

# ─── Executable ──────────────────────────────────────────────────────
add_executable(neon-robot-cef
    src/main.cpp
    src/cuda/cuda_kernel.cpp
    src/vk/vulkan_context.cpp
    src/vk/sampled_image.cpp
    src/vk/shader.cpp
    src/vk/command_buffer.cpp
    src/camera/orbit_camera.cpp)
add_dependencies(neon-robot-cef neon_robot_cef_shaders)

target_include_directories(neon-robot-cef PRIVATE
    "${CEF_ROOT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${Vulkan_INCLUDE_DIRS}
    ${CUDAToolkit_INCLUDE_DIRS}
    ${unordered_dense_SOURCE_DIR}/include)

target_link_libraries(neon-robot-cef PRIVATE
    libcef_dll_wrapper
    debug     "${CEF_LIB_DEBUG}"
    optimized "${CEF_LIB_RELEASE}"
    Vulkan::Vulkan
    glfw
    volk
    glm::glm
    CUDA::cuda_driver
    CUDA::nvrtc
    ovrtx::ovrtx
    unordered_dense::unordered_dense)

# CEF's cmake exposes this macro to set the right runtime/manifest/etc
# attributes on the target (e.g. STATIC_LIBRARY for libcef wrapper).
SET_EXECUTABLE_TARGET_PROPERTIES(neon-robot-cef)

# ─── Post-build: stage runtime alongside the binary ──────────────────
# Linux installs CEF runtime under Release/ and Resources/ inside the
# tarball; Windows uses Release/ and Resources/ too. The CEF Resources
# folder contains .pak and locales/ that the browser process opens at
# startup — they must be next to the exe.
add_custom_command(TARGET neon-robot-cef POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Release"   "$<TARGET_FILE_DIR:neon-robot-cef>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Resources" "$<TARGET_FILE_DIR:neon-robot-cef>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/ui-html"
        "$<TARGET_FILE_DIR:neon-robot-cef>/ui-html"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:neon-robot-cef>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${COMPILED_SHADERS}
        "$<TARGET_FILE_DIR:neon-robot-cef>/shaders/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/neon-only.usda"
        "$<TARGET_FILE_DIR:neon-robot-cef>/"
    COMMENT "Staging CEF runtime + ui-html + shaders + USD next to exe")

# ovrtx provides a helper that copies its DLL/.so files and sets rpath
# so the binary can find libovrtx-dynamic at runtime. Must come last
# so it sees the staged runtime tree.
ovrtx_setup_runtime(neon-robot-cef)
