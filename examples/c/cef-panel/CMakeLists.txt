cmake_minimum_required(VERSION 3.21)
project(cef-panel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- CEF binary distribution ---------------------------------------------
set(CEF_VERSION "147.0.10+gd58e84d+chromium-147.0.7727.118")
set(CEF_PLATFORM "windows64")
string(REPLACE "+" "%2B" CEF_VERSION_URL "${CEF_VERSION}")

set(CEF_DIR "${CMAKE_BINARY_DIR}/cef_binary_${CEF_VERSION}_${CEF_PLATFORM}")

if(NOT EXISTS "${CEF_DIR}/CMakeLists.txt")
    set(CEF_TARBALL "${CMAKE_BINARY_DIR}/cef_binary_${CEF_VERSION}_${CEF_PLATFORM}.tar.bz2")
    set(CEF_URL "https://cef-builds.spotifycdn.com/cef_binary_${CEF_VERSION_URL}_${CEF_PLATFORM}.tar.bz2")
    message(STATUS "Downloading CEF: ${CEF_URL}")
    file(DOWNLOAD "${CEF_URL}" "${CEF_TARBALL}" SHOW_PROGRESS STATUS DL_STATUS)
    list(GET DL_STATUS 0 DL_CODE)
    if(NOT DL_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download CEF: ${DL_STATUS}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${CEF_TARBALL}" DESTINATION "${CMAKE_BINARY_DIR}")
endif()

set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "" FORCE)
list(APPEND CMAKE_MODULE_PATH "${CEF_DIR}/cmake")
set(CEF_ROOT "${CEF_DIR}")
find_package(CEF REQUIRED)
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

if(NOT CEF_TARGET_OUT_DIR)
    set(CEF_TARGET_OUT_DIR "${CMAKE_BINARY_DIR}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CEF_TARGET_OUT_DIR}")
endif()

# --- Bench binary --------------------------------------------------------
add_executable(cef-panel
    src/main.cpp
    src/cef_app.cpp
    src/cef_client.cpp)

target_include_directories(cef-panel PRIVATE src ${CEF_DIR})
target_link_libraries(cef-panel PRIVATE
    libcef_lib
    libcef_dll_wrapper
    ${CEF_STANDARD_LIBS})

SET_EXECUTABLE_TARGET_PROPERTIES(cef-panel)

# Console subsystem so we can see stdout / stderr while spiking.
if(MSVC)
    target_link_options(cef-panel PRIVATE /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup)
endif()

COPY_FILES(cef-panel "${CEF_BINARY_FILES}"   "${CEF_BINARY_DIR}"   "${CEF_TARGET_OUT_DIR}")
COPY_FILES(cef-panel "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")

# HTML test pages — default URL points at test_pages/neon.html.
add_custom_command(TARGET cef-panel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/test_pages"
            "$<TARGET_FILE_DIR:cef-panel>/test_pages")
