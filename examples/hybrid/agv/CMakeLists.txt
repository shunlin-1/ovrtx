# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Hybrid (C++ UI + Python helpers) port of examples/python/agv.
#
# C++ side: Qt6 + ovrtx C API. Owns the renderer, render thread, UI
#          slots/signals, and ray/material logic at runtime.
# Python side: short-lived `uv run` subprocesses for things that need
#          USD (pick_collector_bin.py) or future AI libs.
#
# The two languages never share a process — Python is always launched
# as a child so its usd-core import doesn't collide with ovrtx's
# bundled USD. See README.md for the call diagram.

cmake_minimum_required(VERSION 3.18)
project(agv LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FETCHCONTENT_QUIET FALSE)
# Share fetched deps with examples/c/* so the ~hundreds-of-MB ovrtx tarball
# is only downloaded once across all example projects.
set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../c/_deps" CACHE PATH "")

# ── ovrtx ─────────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../c/cmake")
include(ovrtx)
ovrtx_fetch()

# ── Qt6 ───────────────────────────────────────────────────────────────
# On Debian/Ubuntu: sudo apt install qt6-base-dev qt6-declarative-dev
# On Arch:          sudo pacman -S qt6-base qt6-declarative
# Qt 6.5+ required: GlassButton + DraggablePanel use QtQuick.Effects.MultiEffect
# which was added in 6.5. On distros stuck at 6.4 (e.g. Ubuntu 24.04 LTS main),
# install via aqtinstall and pass -DCMAKE_PREFIX_PATH=~/Qt/6.8.x/gcc_64.
find_package(Qt6 6.5 COMPONENTS Core Gui Qml Quick REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ── Default USD path (the existing AGV scene from the Python example) ─
# Overridable at runtime via --usd PATH.
set(AGV_DEFAULT_USD "${CMAKE_CURRENT_LIST_DIR}/../../python/agv/Test.usda"
    CACHE PATH "Default USD scene loaded if --usd is not passed")

# ── Optional Python embedding (AI worker, future) ─────────────────────
option(AGV_ENABLE_PYTHON "Embed CPython for AI workers (off by default)" OFF)
if(AGV_ENABLE_PYTHON)
    find_package(Python3 3.10 COMPONENTS Development Interpreter REQUIRED)
    message(STATUS "agv: Python embedding ENABLED (${Python3_VERSION})")
endif()

# ── Sources ───────────────────────────────────────────────────────────
add_executable(agv
    src/main.cpp
    src/sidecar.cpp
    src/orbit_camera.cpp
    src/frame_image_provider.cpp
    src/agv_backend.cpp
    src/pick_table.cpp
    src/material_overrides.cpp
)

# Compile-time paths. main.cpp resolves QML + pick_collector from here
# unless overridden by CLI flags. Embedding via Qt resources is a later
# optimization — for a dev scaffold we want hot QML / Python edits
# without recompiles.
target_compile_definitions(agv PRIVATE
    AGV_DEFAULT_USD="${AGV_DEFAULT_USD}"
    AGV_QML_DIR="${CMAKE_CURRENT_LIST_DIR}/qml"
    AGV_PICK_COLLECTOR="${CMAKE_CURRENT_LIST_DIR}/pick_collector_bin.py"
)

# Bake the Qt runtime paths into the binary so `./build/agv` works
# without LD_LIBRARY_PATH / QML_IMPORT_PATH env vars. Qt6_DIR points at
# <prefix>/lib/cmake/Qt6, so the prefix is two levels up.
get_filename_component(_agv_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
set_target_properties(agv PROPERTIES
    BUILD_RPATH   "${_agv_qt_prefix}/lib"
    INSTALL_RPATH "${_agv_qt_prefix}/lib"
)
target_compile_definitions(agv PRIVATE
    AGV_QT_QML_DIR="${_agv_qt_prefix}/qml"
)

target_link_libraries(agv PRIVATE
    ovrtx::ovrtx
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
)

if(AGV_ENABLE_PYTHON)
    # TODO: drop python_bridge.cpp here when the AI worker is added.
    target_link_libraries(agv PRIVATE Python3::Python)
    target_compile_definitions(agv PRIVATE AGV_HAS_PYTHON=1)
endif()

# rpath / DLL copying for libovrtx-dynamic.{so,dll}
ovrtx_setup_runtime(agv)
