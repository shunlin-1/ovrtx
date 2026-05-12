// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#pragma once

#include <string>

namespace agv {

// Sidecar layer authored at startup. References the user's AGV scene
// and adds /World/Camera (with omni:xform we'll write each frame) plus
// /Render/Camera (the render product ovrtx_step renders into). 1:1
// with examples/python/agv/main.py:write_sidecar.
struct SidecarConfig {
    std::string scene_usd_path;
    std::string out_path;
    std::string rendermode      = "RaytracedLighting";
    char        up_axis         = 'Y';
    double      meters_per_unit = 0.01;
    int         width           = 1920;
    int         height          = 1080;
    double      focal_length         = 18.5;
    double      horizontal_aperture  = 20.955;
    double      vertical_aperture    = 11.787;
};

// Parses upAxis + metersPerUnit from the first 4 KiB of the source USD.
// Defaults to Y-up, cm. Mirrors parse_stage_metadata in the Python.
struct StageMetadata { char up_axis = 'Y'; double meters_per_unit = 0.01; };
StageMetadata parse_stage_metadata(const std::string& usd_path);

// Writes the sidecar USDA to out_path. Returns true on success.
bool write_sidecar(const SidecarConfig& cfg);

}  // namespace agv
