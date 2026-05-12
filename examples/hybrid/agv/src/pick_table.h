// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace agv {

// One mesh's broadphase entry. v2 binary format carries AABBs +
// bound-shader path + material name. Triangle data still omitted
// (deferred to v3 for the narrowphase pass).
// See pick_collector_bin.py for the writer.
struct PickEntry {
    std::string             mesh_path;
    std::string             shader_path;
    std::string             material_name;     // v2 — human-readable
    std::array<double, 3>   bb_min{};
    std::array<double, 3>   bb_max{};
    std::array<float, 3>    orig_color{};
    float                   orig_intensity = 0.0f;
};

// Reads the binary pick file produced by pick_collector_bin.py.
// Returns an empty vector on any I/O / format error and logs to stderr.
std::vector<PickEntry> load_pick_table(const std::string& bin_path);

}  // namespace agv
