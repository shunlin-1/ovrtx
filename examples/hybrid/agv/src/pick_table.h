// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace agv {

// One mesh's pick entry. v3 carries AABBs (broadphase) + per-triangle
// world-space vertex arrays (narrowphase). Together they enable a
// proper "ray-AABB → ray-triangle" pick that lands on the visible
// surface rather than whatever enveloping bbox happens to be hit first.
// See pick_collector_bin.py for the writer.
struct PickEntry {
    std::string             mesh_path;
    std::string             shader_path;
    std::string             material_name;
    std::array<double, 3>   bb_min{};
    std::array<double, 3>   bb_max{};
    std::array<float, 3>    orig_color{};
    float                   orig_intensity = 0.0f;

    // v3 — world-space triangles. Parallel arrays: triangle i has
    // vertices v0[i], v1[i], v2[i]. Stored interleaved-by-attribute
    // (three SoA arrays) so the ray-triangle inner loop is cache-
    // friendly and SIMD-friendly if we want to vectorise later.
    std::vector<std::array<float, 3>> v0;
    std::vector<std::array<float, 3>> v1;
    std::vector<std::array<float, 3>> v2;
};

// Reads the binary pick file produced by pick_collector_bin.py.
// Returns an empty vector on any I/O / format error and logs to stderr.
std::vector<PickEntry> load_pick_table(const std::string& bin_path);

}  // namespace agv
