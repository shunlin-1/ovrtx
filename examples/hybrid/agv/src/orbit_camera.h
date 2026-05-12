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

    // Aim the orbit pivot at an explicit world-space point. Backend
    // computes this from the scene-bbox center so the camera frames
    // off-origin assets (e.g. BIM building exports).
    void set_target(double x, double y, double z) {
        target_x_ = x; target_y_ = y; target_z_ = z;
    }

    // Override the default zoom clamp. Default 0.1..500 is tuned for
    // cm-scale AGV assets; meter or mm scenes need different limits.
    void set_distance_limits(double min_d, double max_d) {
        min_distance_ = min_d;
        max_distance_ = max_d;
    }

    // Set the current distance directly (used to re-frame after
    // computing scene extent).
    void set_distance(double d) { distance_ = d; }

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
    double min_distance_ = 0.1;     // overrideable via set_distance_limits
    double max_distance_ = 500.0;
};

}  // namespace agv
