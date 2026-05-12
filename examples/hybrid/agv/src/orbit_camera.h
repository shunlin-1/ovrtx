// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#pragma once

#include <array>
#include <string>

namespace agv {

// Row-vector 4x4 matrix, USD/ovrtx convention: translation in row 3.
// Stored row-major as 16 doubles; matrix[r*4 + c].
using Mat4d = std::array<double, 16>;

// Orbit camera that mirrors examples/python/agv/main.py:OrbitCamera.
// Up-axis configurable so it handles both Y-up (AGV Test.usda) and
// Z-up scenes without rewriting the camera basis math.
class OrbitCamera {
public:
    OrbitCamera(double distance = 5.0,
                double azimuth_rad = 0.610865,    // 35°
                double elevation_rad = 0.349066,  // 20°
                char up_axis = 'Y');

    void orbit(double dx_pixels, double dy_pixels);
    void zoom(double ticks);

    // Row-vector world transform (omni:xform).
    Mat4d matrix() const;

    char up_axis() const { return up_axis_; }
    double distance() const { return distance_; }

private:
    double target_x_ = 0.0, target_y_ = 0.0, target_z_ = 0.0;
    double distance_;
    double azimuth_;     // radians
    double elevation_;   // radians, clamped to ±89°
    char up_axis_;       // 'Y' or 'Z'
};

}  // namespace agv
