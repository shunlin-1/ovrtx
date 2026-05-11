# CEF + ovrtx integration retest.
#
# Phases (one binary, mode selected at runtime by flag):
#   default        -> Phase 1: headless OSR -> out.png snapshot
#   --stability    -> Phase 2: continuous OSR loop with paint+uptime log
#   --vulkan       -> Phase 3: GLFW window + Vulkan composite, CEF OSR
#                              uploaded each frame to a sampled image
#                              and drawn through the same shader the
#                              Ultralight integration in neon-robot-c uses
#
# Phase 3 reuses the Vulkan/shader/sampled-image classes from the
# neon-robot-c sample (CUDA dependency stubbed out — see vulkan_context.hpp).

cmake_minimum_required(VERSION 3.21)
project(cef-bim-test LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# CEF 147 defaults to /MT (static CRT). Match that across the whole
# build so libcef_dll_wrapper, GLFW, and the exe all share one CRT —
# mixing /MT and /MD causes unresolved __imp_strtok / __imp_strncpy at
# link time because they expect different stdlib symbol shapes.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

include(FetchContent)

# ── Phase 3 toolchain (Vulkan + GLFW + glm + volk + unordered_dense) ──
find_package(Vulkan REQUIRED)
find_program(GLSLC_EXECUTABLE glslc HINTS $ENV{VULKAN_1_2_SDK_BIN})
if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found. Install Vulkan SDK.")
endif()
message(STATUS "Found glslc: ${GLSLC_EXECUTABLE}")

set(FETCHCONTENT_QUIET FALSE)
# Share fetched deps with sibling projects (../ovrtx/examples/c/_deps).
set(FETCHCONTENT_BASE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../ovrtx/examples/c/_deps"
    CACHE PATH "")

FetchContent_Declare(glfw3
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# Force GLFW to use /MT to match CEF's CRT — mixing CRT shapes causes
# unresolved __imp_strtok / __imp_fmaxf symbols at link time.
set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw3)

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

# ── CEF binary distribution ──────────────────────────────────────────
set(CEF_VERSION  "147.0.10+gd58e84d+chromium-147.0.7727.118")
set(CEF_PLATFORM "windows64")

string(REPLACE "+" "%2B" CEF_VERSION_URL "${CEF_VERSION}")
set(CEF_DIR     "${CMAKE_BINARY_DIR}/cef_binary_${CEF_VERSION}_${CEF_PLATFORM}")
set(CEF_TARBALL "${CMAKE_BINARY_DIR}/cef.tar.bz2")
set(CEF_URL     "https://cef-builds.spotifycdn.com/cef_binary_${CEF_VERSION_URL}_${CEF_PLATFORM}.tar.bz2")

if(NOT EXISTS "${CEF_DIR}/cmake/cef_variables.cmake")
    if(NOT EXISTS "${CEF_TARBALL}")
        message(STATUS "Downloading CEF ${CEF_VERSION} (~150 MB)...")
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

# ── Compile shaders to SPIR-V at build time ─────────────────────────
set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

set(SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.frag"
)
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
add_custom_target(cef_bim_shaders ALL DEPENDS ${COMPILED_SHADERS})

# ── Our app ──────────────────────────────────────────────────────────
add_executable(cef-bim-test
    src/main.cpp
    src/vk/vulkan_context.cpp
    src/vk/sampled_image.cpp
    src/vk/shader.cpp
    src/vk/command_buffer.cpp
)
add_dependencies(cef-bim-test cef_bim_shaders)

target_include_directories(cef-bim-test PRIVATE
    "${CEF_ROOT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(cef-bim-test PRIVATE
    libcef_dll_wrapper
    debug     "${CEF_LIB_DEBUG}"
    optimized "${CEF_LIB_RELEASE}"
    Vulkan::Vulkan
    glfw
    volk
    glm::glm
    unordered_dense::unordered_dense
)

SET_EXECUTABLE_TARGET_PROPERTIES(cef-bim-test)

# Copy CEF runtime + UI assets + compiled shaders next to the binary.
add_custom_command(TARGET cef-bim-test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Release"   "$<TARGET_FILE_DIR:cef-bim-test>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Resources" "$<TARGET_FILE_DIR:cef-bim-test>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/ui-html"
        "$<TARGET_FILE_DIR:cef-bim-test>/ui-html"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/test_pages"
        "$<TARGET_FILE_DIR:cef-bim-test>/test_pages"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${SHADER_OUT_DIR}"
        "$<TARGET_FILE_DIR:cef-bim-test>/shaders"
    COMMENT "Copying CEF runtime + ui-html + test_pages + shaders"
)
