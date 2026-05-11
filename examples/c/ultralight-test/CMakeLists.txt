cmake_minimum_required(VERSION 3.21)
project(ultralight-test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Download Ultralight SDK ────────────────────────────────────────────
# Official CDN: ultralight-sdk-latest-{platform}-{arch}.7z
# For Windows x64 we use the "latest" alias; archived builds also live
# under https://ultralight-sdk.sfo2.cdn.digitaloceanspaces.com/.
set(UL_SDK_URL  "https://ultralight-sdk.sfo2.cdn.digitaloceanspaces.com/ultralight-sdk-latest-win-x64.7z")
set(UL_SDK_DIR  "${CMAKE_BINARY_DIR}/ultralight-sdk")
set(UL_SDK_FILE "${CMAKE_BINARY_DIR}/ultralight-sdk-latest-win-x64.7z")

if(NOT EXISTS "${UL_SDK_DIR}/include")
    if(NOT EXISTS "${UL_SDK_FILE}")
        message(STATUS "Downloading Ultralight SDK from ${UL_SDK_URL}")
        file(DOWNLOAD "${UL_SDK_URL}" "${UL_SDK_FILE}"
             SHOW_PROGRESS STATUS UL_DL_STATUS)
        list(GET UL_DL_STATUS 0 UL_DL_CODE)
        if(NOT UL_DL_CODE EQUAL 0)
            message(FATAL_ERROR "Ultralight SDK download failed: ${UL_DL_STATUS}")
        endif()
    endif()
    message(STATUS "Extracting Ultralight SDK...")
    file(MAKE_DIRECTORY "${UL_SDK_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${UL_SDK_FILE}" DESTINATION "${UL_SDK_DIR}")
endif()

# Sanity check: SDK layout typically has include/, bin/, lib/.
if(NOT EXISTS "${UL_SDK_DIR}/include/Ultralight")
    message(FATAL_ERROR
        "Ultralight SDK extracted to ${UL_SDK_DIR} but expected layout "
        "(include/Ultralight) is missing. Check the .7z extraction.")
endif()

# ─── Test executable ────────────────────────────────────────────────────
add_executable(ultralight-test src/main.cpp)

target_include_directories(ultralight-test PRIVATE
    "${UL_SDK_DIR}/include")

# Static-import libs sit in lib/, runtime DLLs in bin/.
target_link_directories(ultralight-test PRIVATE "${UL_SDK_DIR}/lib")
target_link_libraries(ultralight-test PRIVATE
    Ultralight
    UltralightCore
    WebCore
    AppCore)

# Copy Ultralight runtime files (DLLs + ICU + cacert + assets) next to
# the exe. The `resources/` directory contains icudt67l.dat which the
# WebCore engine loads at startup — without it Ultralight aborts during
# Renderer::Create with "Failed to load ICU data".
add_custom_command(TARGET ultralight-test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${UL_SDK_DIR}/bin"
            "$<TARGET_FILE_DIR:ultralight-test>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${UL_SDK_DIR}/resources"
            "$<TARGET_FILE_DIR:ultralight-test>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/content/index.html"
            "$<TARGET_FILE_DIR:ultralight-test>/index.html"
    COMMENT "Copying Ultralight runtime files + test HTML")

if(MSVC)
    # Match Ultralight's CRT (it's built with /MD).
    set_property(TARGET ultralight-test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()
