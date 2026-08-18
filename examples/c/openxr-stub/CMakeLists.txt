# SPDX-License-Identifier: MIT
#
# Builds the OpenXR probe. The OpenXR-SDK (headers + loader) is fetched at
# configure time, so no system package is required — matching how ovrtx and
# ovstream fetch their own binaries in their example projects.

cmake_minimum_required(VERSION 3.20)
project(ovrtx_openxr_stub C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# The loader is what makes this build-but-not-run: it links fine and only
# looks for an actual runtime at xrCreateInstance time.
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)

# pkg-config reports the xcb module as present from libxcb1-dev alone, so the
# SDK turns on XR_USE_PLATFORM_XCB and then fails to find <xcb/glx.h>, which
# only ships in libxcb-glx0-dev. An ovrtx XR app is Vulkan-only and never
# touches the GLX structs, so switch XCB off rather than adding an apt
# dependency. Xlib stays on because the SDK's presentation backend defaults
# to it and refuses to configure without it.
set(BUILD_WITH_XCB_HEADERS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    OpenXR
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.43
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(OpenXR)

add_executable(openxr_stub main.c)
target_link_libraries(openxr_stub PRIVATE OpenXR::openxr_loader)
