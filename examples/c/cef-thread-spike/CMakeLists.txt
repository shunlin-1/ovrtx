cmake_minimum_required(VERSION 3.21)

project(cef-thread-spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CEF version pinned. Bump only when we have a reason.
set(CEF_VERSION "147.0.10+gd58e84d+chromium-147.0.7727.118")
set(CEF_PLATFORM "windows64")
string(REPLACE "+" "%2B" CEF_VERSION_URL "${CEF_VERSION}")

set(CEF_DIR "${CMAKE_BINARY_DIR}/cef_binary_${CEF_VERSION}_${CEF_PLATFORM}")

if(NOT EXISTS "${CEF_DIR}/CMakeLists.txt")
    set(CEF_TARBALL "${CMAKE_BINARY_DIR}/cef_binary_${CEF_VERSION}_${CEF_PLATFORM}.tar.bz2")
    set(CEF_URL "https://cef-builds.spotifycdn.com/cef_binary_${CEF_VERSION_URL}_${CEF_PLATFORM}.tar.bz2")

    message(STATUS "Downloading CEF binary distribution from: ${CEF_URL}")
    file(DOWNLOAD "${CEF_URL}" "${CEF_TARBALL}"
         SHOW_PROGRESS
         STATUS DOWNLOAD_STATUS)
    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
    if(NOT DOWNLOAD_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download CEF: ${DOWNLOAD_STATUS}")
    endif()

    message(STATUS "Extracting CEF...")
    file(ARCHIVE_EXTRACT INPUT "${CEF_TARBALL}" DESTINATION "${CMAKE_BINARY_DIR}")
endif()

# CEF's bundled CMake config exposes the helper macros and CEF_* variables.
list(APPEND CMAKE_MODULE_PATH "${CEF_DIR}/cmake")
set(CEF_ROOT "${CEF_DIR}")
find_package(CEF REQUIRED)

# Build CEF's C++ wrapper (libcef_dll_wrapper) as a static lib. The wrapper
# source lives under ${CEF_DIR}/libcef_dll in current CEF distributions; the
# CEF cmake helpers set CEF_LIBCEF_DLL_WRAPPER_PATH to point at it.
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)

# Create the imported `libcef_lib` target wrapping libcef.lib (Windows) or
# libcef.so (Linux). cefsimple example uses this exact pattern.
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

# CEF's SET_CEF_TARGET_OUT_DIR macro only handles Ninja/Unix Makefiles. With
# NMake Makefiles (Windows console build) it never sets CEF_TARGET_OUT_DIR, so
# COPY_FILES silently no-ops. Set it explicitly here.
set(CEF_TARGET_OUT_DIR "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CEF_TARGET_OUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CEF_TARGET_OUT_DIR}")

# --- Our spike binary ----------------------------------------------------

set(SPIKE_SRCS
    src/main.cpp
    src/cef_app.cpp
    src/cef_client.cpp
    src/render_thread.cpp
)

add_executable(cef-thread-spike ${SPIKE_SRCS})

target_include_directories(cef-thread-spike PRIVATE
    src
    ${CEF_DIR}
)

target_link_libraries(cef-thread-spike PRIVATE
    libcef_lib
    libcef_dll_wrapper
    ${CEF_STANDARD_LIBS}
)

# CEF's helper macros set required compile/link flags (subsystem, sandbox, manifests).
SET_EXECUTABLE_TARGET_PROPERTIES(cef-thread-spike)

# Copy CEF runtime files (libcef.dll, *.pak, locales/, swiftshader/) next to
# the .exe. Use CEF_TARGET_OUT_DIR which CEF's cmake helpers set up for us.
COPY_FILES(cef-thread-spike
    "${CEF_BINARY_FILES}"
    "${CEF_BINARY_DIR}"
    "${CEF_TARGET_OUT_DIR}"
)
COPY_FILES(cef-thread-spike
    "${CEF_RESOURCE_FILES}"
    "${CEF_RESOURCE_DIR}"
    "${CEF_TARGET_OUT_DIR}"
)

# Copy our test page next to the .exe so file:// URLs work.
add_custom_command(TARGET cef-thread-spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/test_pages"
            "${CEF_TARGET_OUT_DIR}/test_pages"
)
