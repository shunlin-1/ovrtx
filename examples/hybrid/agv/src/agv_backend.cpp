// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#include "agv_backend.h"

#include "frame_image_provider.h"
#include "material_overrides.h"

#include <ovrtx/ovrtx.h>
#include <ovrtx/ovrtx_config.h>
#include <ovrtx/ovrtx_types.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string_view>

namespace agv {

namespace {

template <typename ResultT>
bool check_ovrtx(ResultT const& r, std::string_view op) {
    if (r.status != OVRTX_API_ERROR) return false;
    ovx_string_t e = ovrtx_get_last_error();
    if (e.ptr && e.length > 0) {
        std::fprintf(stderr, "ovrtx %.*s failed: %.*s\n",
                     int(op.size()), op.data(),
                     int(e.length), e.ptr);
    } else {
        std::fprintf(stderr, "ovrtx %.*s failed\n",
                     int(op.size()), op.data());
    }
    return true;
}

// Lens parameters — must match sidecar.cpp's defaults so screen-pixel
// to world-ray math stays in sync. Camera-space ray dx/dy use these.
constexpr double kFocalLength       = 18.5;
constexpr double kHorizontalAperture = 20.955;
constexpr double kVerticalAperture   = 11.787;

// Slab method ray-AABB intersection. Returns t > 0 along the ray if
// hit, else infinity. `dir` need not be normalised — t is in units of
// |dir|. Mirrors ray_aabb() in examples/python/agv/main.py.
double ray_aabb(const double eye[3], const double dir[3],
                const std::array<double, 3>& bb_min,
                const std::array<double, 3>& bb_max) {
    double tmin = -std::numeric_limits<double>::infinity();
    double tmax =  std::numeric_limits<double>::infinity();
    for (int i = 0; i < 3; ++i) {
        double d = std::fabs(dir[i]) < 1e-12
                     ? std::copysign(1e-12, dir[i])
                     : dir[i];
        double t1 = (bb_min[i] - eye[i]) / d;
        double t2 = (bb_max[i] - eye[i]) / d;
        double lo = std::min(t1, t2), hi = std::max(t1, t2);
        if (lo > tmin) tmin = lo;
        if (hi < tmax) tmax = hi;
    }
    if (tmax < std::max(tmin, 0.0)) {
        return std::numeric_limits<double>::infinity();
    }
    return tmin > 0.0 ? tmin : tmax;
}

ovrtx_rendered_output_handle_t find_ldr_color(
    ovrtx_render_product_set_outputs_t const& outputs) {
    for (size_t i = 0; i < outputs.output_count; ++i) {
        const auto& product = outputs.outputs[i];
        for (size_t f = 0; f < product.output_frame_count; ++f) {
            const auto& frame = product.output_frames[f];
            for (size_t v = 0; v < frame.render_var_count; ++v) {
                const auto& var = frame.output_render_vars[v];
                if (var.render_var_name.ptr &&
                    std::strncmp(var.render_var_name.ptr,
                                 "LdrColor",
                                 var.render_var_name.length) == 0) {
                    return var.output_handle;
                }
            }
        }
    }
    return -1;
}

}  // namespace

AgvBackend::AgvBackend(FrameImageProvider* provider,
                       std::string sidecar_usd_path,
                       char up_axis,
                       double initial_distance,
                       std::vector<PickEntry> pick_table,
                       QObject* parent)
    : QObject(parent),
      provider_(provider),
      sidecar_path_(std::move(sidecar_usd_path)),
      camera_(initial_distance, /*az=*/0.610865, /*el=*/0.349066, up_axis),
      pick_table_(std::move(pick_table)) {
    // pick_table_ is fully populated before the worker starts, so the
    // worker can read it lock-free for the rest of its lifetime.
    worker_ = std::thread(&AgvBackend::runWorker, this);
}

AgvBackend::~AgvBackend() { stop(); }

void AgvBackend::stop() {
    stop_requested_.store(true);
    if (worker_.joinable()) worker_.join();
}

void AgvBackend::orbit(double dx, double dy) {
    std::lock_guard<std::mutex> g(camera_mutex_);
    camera_.orbit(dx, dy);
    camera_dirty_ = true;
}

void AgvBackend::zoom(double ticks) {
    std::lock_guard<std::mutex> g(camera_mutex_);
    camera_.zoom(ticks);
    camera_dirty_ = true;
}

void AgvBackend::pick(double x_frac, double y_frac) {
    std::lock_guard<std::mutex> g(picks_mutex_);
    pending_picks_.emplace_back(x_frac, y_frac);
}

QString AgvBackend::pickMode() const { return pick_mode_; }

void AgvBackend::setPickMode(const QString& mode) {
    // Update the QString (for QML) and the atomic mirror (for the
    // worker thread) together so they don't drift.
    int code = -1;
    if      (mode == QLatin1String("neon"))       code = 0;
    else if (mode == QLatin1String("xray"))       code = 1;
    else if (mode == QLatin1String("xray-light")) code = 2;
    if (code < 0 || mode == pick_mode_) return;

    pick_mode_ = mode;
    mode_atomic_.store(code, std::memory_order_release);
    emit pickModeChanged();
    std::fprintf(stderr, "[pick] mode -> %s\n", qPrintable(mode));
}

// [snippet:ovrtx-worker-loop]
void AgvBackend::runWorker() {
    ovrtx_renderer_t* renderer = nullptr;
    ovrtx_config_t cfg{};
    if (check_ovrtx(ovrtx_create_renderer(&cfg, &renderer), "create_renderer"))
        return;

    // Load the sidecar — references the AGV scene and authors the
    // camera + render product we're about to drive.
    ovrtx_usd_input_t usd_in{};
    usd_in.usd_file_path = {sidecar_path_.c_str(),
                            sidecar_path_.size()};
    ovrtx_usd_handle_t usd_handle{};
    ovrtx_enqueue_result_t enq =
        ovrtx_add_usd(renderer, usd_in, {"", 0}, &usd_handle);
    if (check_ovrtx(enq, "add_usd")) {
        ovrtx_destroy_renderer(renderer); return;
    }
    ovrtx_op_wait_result_t wait{};
    while (ovrtx_wait_op(renderer, enq.op_index,
                          ovrtx_timeout_t{0}, &wait).status
            == OVRTX_API_TIMEOUT) {
        if (stop_requested_.load()) {
            ovrtx_destroy_renderer(renderer); return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Camera-write descriptor — built once, reused every dirty frame.
    // Pattern matches examples/c/vulkan-interop/src/main.cpp:write-camera-transform.
    ovx_string_t cam_path = {"/World/Camera", std::strlen("/World/Camera")};
    ovrtx_prim_list_t prim_list{};
    prim_list.prim_paths = &cam_path;
    prim_list.num_paths = 1;

    ovrtx_attribute_type_t attr_type{};
    attr_type.dtype = {kDLFloat, 64, 16};   // mat4d packed into 16 lanes
    attr_type.is_array = false;
    attr_type.semantic = OVRTX_SEMANTIC_XFORM_MAT4x4;

    const char* attr_name = "omni:xform";
    ovrtx_binding_desc_t binding{};
    binding.prim_list = prim_list;
    binding.attribute_name.string = {attr_name, std::strlen(attr_name)};
    binding.attribute_type = attr_type;
    binding.prim_mode = OVRTX_BINDING_PRIM_MODE_EXISTING_ONLY;
    binding.flags = OVRTX_BINDING_FLAG_NONE;

    ovrtx_binding_desc_or_handle_t binding_or_handle{};
    binding_or_handle.binding_desc = binding;

    // Render-product set we'll step.
    ovx_string_t rp_str = {"/Render/Camera", std::strlen("/Render/Camera")};
    ovrtx_render_product_set_t rp_set{};
    rp_set.render_products = &rp_str;
    rp_set.num_render_products = 1;

    // Material overrides own no ovrtx state of their own; bindings are
    // constructed inline per write. Cheap to keep local to the worker.
    MaterialOverrides materials(renderer);

    auto last_tick = std::chrono::steady_clock::now();

    while (!stop_requested_.load()) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_tick).count();
        last_tick = now;

        // ── Push camera if UI moved it ──────────────────────────────
        bool dirty = false;
        Mat4d cam_matrix{};
        {
            std::lock_guard<std::mutex> g(camera_mutex_);
            if (camera_dirty_) {
                cam_matrix = camera_.matrix();
                camera_dirty_ = false;
                dirty = true;
            }
        }
        if (dirty) {
            DLTensor tm{};
            tm.data = cam_matrix.data();
            tm.device = {kDLCPU, 0};
            tm.ndim = 1;
            int64_t shape[1] = {1};
            tm.shape = shape;
            tm.dtype = {kDLFloat, 64, 16};

            ovrtx_input_buffer_t in_buf{};
            in_buf.tensors = &tm;
            in_buf.tensor_count = 1;

            auto wr = ovrtx_write_attribute(renderer, &binding_or_handle,
                                            &in_buf, OVRTX_DATA_ACCESS_SYNC);
            if (check_ovrtx(wr, "write_attribute(camera)")) break;
        }

        // ── Drain pending picks; hit-test against the pick table ────
        // Done after the camera write so we use the freshest basis.
        std::vector<std::pair<double, double>> picks_now;
        {
            std::lock_guard<std::mutex> g(picks_mutex_);
            picks_now.swap(pending_picks_);
        }
        if (!picks_now.empty() && !pick_table_.empty()) {
            // Snapshot the current camera basis. cam_matrix may have
            // been populated above (if dirty this frame) or be stale;
            // safer to just re-read under the lock.
            Mat4d use;
            {
                std::lock_guard<std::mutex> g(camera_mutex_);
                use = camera_.matrix();
            }
            // Row-vector layout: row 0 = right, 1 = up, 2 = -forward, 3 = eye.
            double right[3]   = {use[0],  use[1],  use[2]};
            double up_v[3]    = {use[4],  use[5],  use[6]};
            double forward[3] = {-use[8], -use[9], -use[10]};
            double eye[3]     = {use[12], use[13], use[14]};

            const double h_fov_2 = std::atan(kHorizontalAperture
                                             / (2.0 * kFocalLength));
            const double v_fov_2 = std::atan(kVerticalAperture
                                             / (2.0 * kFocalLength));

            int mode_code = mode_atomic_.load(std::memory_order_acquire);
            ShaderOverride::Mode mode =
                (mode_code == 0) ? ShaderOverride::Mode::Neon
              : (mode_code == 1) ? ShaderOverride::Mode::Xray
              : (mode_code == 2) ? ShaderOverride::Mode::XrayLight
              :                    ShaderOverride::Mode::None;

            for (const auto& [x_frac, y_frac] : picks_now) {
                double ndc_x = 2.0 * x_frac - 1.0;
                double ndc_y = 1.0 - 2.0 * y_frac;
                double dx = ndc_x * std::tan(h_fov_2);
                double dy = ndc_y * std::tan(v_fov_2);
                double world_dir[3] = {
                    right[0] * dx + up_v[0] * dy + forward[0],
                    right[1] * dx + up_v[1] * dy + forward[1],
                    right[2] * dx + up_v[2] * dy + forward[2],
                };
                double n = std::sqrt(world_dir[0]*world_dir[0]
                                   + world_dir[1]*world_dir[1]
                                   + world_dir[2]*world_dir[2]);
                if (n < 1e-9) continue;
                world_dir[0] /= n; world_dir[1] /= n; world_dir[2] /= n;

                double best_t = std::numeric_limits<double>::infinity();
                const PickEntry* best = nullptr;
                for (const auto& pe : pick_table_) {
                    double t = ray_aabb(eye, world_dir, pe.bb_min, pe.bb_max);
                    if (t < best_t) { best_t = t; best = &pe; }
                }
                if (best == nullptr ||
                    !std::isfinite(best_t)) {
                    std::fprintf(stderr,
                                 "[pick] miss at (%.3f, %.3f)\n",
                                 x_frac, y_frac);
                    continue;
                }
                std::fprintf(stderr,
                             "[pick] hit: %s (shader=%s)\n",
                             best->mesh_path.c_str(),
                             best->shader_path.c_str());
                materials.apply(*best, mode);
            }
        }

        // ── Step + fetch ────────────────────────────────────────────
        ovrtx_step_result_handle_t step_h = 0;
        auto step_enq = ovrtx_step(renderer, rp_set, dt, &step_h);
        if (check_ovrtx(step_enq, "step")) break;

        auto step_wait = ovrtx_wait_op(renderer, step_enq.op_index,
                                       ovrtx_timeout_infinite, &wait);
        if (check_ovrtx(step_wait, "wait(step)")) {
            ovrtx_destroy_results(renderer, step_h);
            break;
        }

        ovrtx_render_product_set_outputs_t outputs{};
        auto fetch = ovrtx_fetch_results(renderer, step_h,
                                         ovrtx_timeout_infinite, &outputs);
        if (check_ovrtx(fetch, "fetch_results")) {
            ovrtx_destroy_results(renderer, step_h);
            break;
        }

        auto ldr_h = find_ldr_color(outputs);
        if (ldr_h != -1) {
            ovrtx_map_output_description_t map_desc{};
            map_desc.device_type = OVRTX_MAP_DEVICE_TYPE_CPU;
            ovrtx_rendered_output_t rendered{};
            auto map_r = ovrtx_map_rendered_output(
                renderer, ldr_h, &map_desc, ovrtx_timeout_infinite, &rendered);

            if (!check_ovrtx(map_r, "map_rendered_output")) {
                const DLTensor& t = rendered.buffer.dl;
                int height = static_cast<int>(t.shape[0]);
                int width  = static_cast<int>(t.shape[1]);
                provider_->update(static_cast<const std::uint8_t*>(t.data),
                                  width, height);

                ovrtx_cuda_sync_t no_sync{};
                ovrtx_unmap_rendered_output(renderer, rendered.map_handle, no_sync);

                int next = frame_counter_.fetch_add(1) + 1;
                // QueuedConnection across threads is automatic when the
                // receiver is QObject-owned by the main thread.
                emit frameChanged(next);
            }
        }

        ovrtx_destroy_results(renderer, step_h);
    }

    ovrtx_destroy_renderer(renderer);
}
// [/snippet:ovrtx-worker-loop]

}  // namespace agv
