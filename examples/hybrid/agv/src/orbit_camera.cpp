// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#include "orbit_camera.h"

#include <algorithm>
#include <cmath>

namespace agv {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kElevClampRad = 89.0 * kPi / 180.0;
constexpr double kOrbitSens = 0.005;

inline void normalize3(double v[3]) {
    double n = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (n > 1e-9) { v[0] /= n; v[1] /= n; v[2] /= n; }
}
inline void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}
}  // namespace

OrbitCamera::OrbitCamera(double distance, double azimuth_rad,
                         double elevation_rad, char up_axis)
    : distance_(distance), azimuth_(azimuth_rad),
      elevation_(elevation_rad),
      up_axis_(up_axis == 'Z' ? 'Z' : 'Y') {}

// [snippet:orbit-input]
void OrbitCamera::orbit(double dx_pixels, double dy_pixels) {
    azimuth_   -= dx_pixels * kOrbitSens;
    elevation_ += dy_pixels * kOrbitSens;
    elevation_ = std::clamp(elevation_, -kElevClampRad, kElevClampRad);
}

void OrbitCamera::zoom(double ticks) {
    distance_ *= std::pow(0.9, ticks);
    distance_ = std::clamp(distance_, min_distance_, max_distance_);
}
// [/snippet:orbit-input]

// [snippet:orbit-camera-matrix]
Mat4d OrbitCamera::matrix() const {
    double ce = std::cos(elevation_), se = std::sin(elevation_);
    double ca = std::cos(azimuth_),   sa = std::sin(azimuth_);

    // Eye position depends on which axis is "up". Standard spherical
    // parameterization, branched per up-axis. Mirrors the Python impl
    // in examples/python/agv/main.py:OrbitCamera.matrix.
    double offset[3];
    double world_up[3];
    if (up_axis_ == 'Z') {
        offset[0] = distance_ * ce * ca;
        offset[1] = distance_ * ce * sa;
        offset[2] = distance_ * se;
        world_up[0] = 0.0; world_up[1] = 0.0; world_up[2] = 1.0;
    } else {  // 'Y'
        offset[0] = distance_ * ce * ca;
        offset[1] = distance_ * se;
        offset[2] = distance_ * ce * sa;
        world_up[0] = 0.0; world_up[1] = 1.0; world_up[2] = 0.0;
    }
    double eye[3] = {
        target_x_ + offset[0], target_y_ + offset[1], target_z_ + offset[2]};

    double forward[3] = {
        target_x_ - eye[0], target_y_ - eye[1], target_z_ - eye[2]};
    normalize3(forward);

    double right[3];
    cross3(forward, world_up, right);
    double rn = std::sqrt(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
    if (rn < 1e-6) { right[0] = 1.0; right[1] = 0.0; right[2] = 0.0; }
    else { right[0] /= rn; right[1] /= rn; right[2] /= rn; }

    double up[3];
    cross3(right, forward, up);

    // Row-vector layout. Camera looks down its local -Z.
    Mat4d m{};
    m[0]  = right[0];   m[1]  = right[1];   m[2]  = right[2];   m[3]  = 0.0;
    m[4]  = up[0];      m[5]  = up[1];      m[6]  = up[2];      m[7]  = 0.0;
    m[8]  = -forward[0];m[9]  = -forward[1];m[10] = -forward[2];m[11] = 0.0;
    m[12] = eye[0];     m[13] = eye[1];     m[14] = eye[2];     m[15] = 1.0;
    return m;
}
// [/snippet:orbit-camera-matrix]

}  // namespace agv
