// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#include "agv_backend.h"

#include "frame_image_provider.h"
#include "material_overrides.h"

#include <ovrtx/ovrtx.h>
#include <ovrtx/ovrtx_config.h>
#include <ovrtx/ovrtx_types.h>

#include <algorithm>
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
// Möller-Trumbore ray-triangle test. Returns the minimum t > EPS where
// the ray hits any of the N triangles in the parallel V0/V1/V2 arrays,
// or +inf if no hit. `dir` need not be normalised — `t` is in units
// of |dir|. Same algorithm as examples/python/agv/main.py:ray_triangles_min_t.
double ray_triangles_min_t(const double eye[3], const double dir[3],
                           const std::vector<std::array<float, 3>>& V0,
                           const std::vector<std::array<float, 3>>& V1,
                           const std::vector<std::array<float, 3>>& V2) {
    constexpr double kEps = 1e-7;
    double best_t = std::numeric_limits<double>::infinity();
    const std::size_t n = V0.size();
    for (std::size_t i = 0; i < n; ++i) {
        // edge1 = V1 - V0,   edge2 = V2 - V0
        const double e1x = V1[i][0] - V0[i][0];
        const double e1y = V1[i][1] - V0[i][1];
        const double e1z = V1[i][2] - V0[i][2];
        const double e2x = V2[i][0] - V0[i][0];
        const double e2y = V2[i][1] - V0[i][1];
        const double e2z = V2[i][2] - V0[i][2];
        // h = cross(dir, edge2)
        const double hx = dir[1]*e2z - dir[2]*e2y;
        const double hy = dir[2]*e2x - dir[0]*e2z;
        const double hz = dir[0]*e2y - dir[1]*e2x;
        // a = dot(edge1, h) — parallel ray rejected on near-zero a.
        const double a = e1x*hx + e1y*hy + e1z*hz;
        if (std::fabs(a) < kEps) continue;
        const double f = 1.0 / a;
        // s = eye - V0
        const double sx = eye[0] - V0[i][0];
        const double sy = eye[1] - V0[i][1];
        const double sz = eye[2] - V0[i][2];
        // u = f * dot(s, h)
        const double u = f * (sx*hx + sy*hy + sz*hz);
        if (u < 0.0 || u > 1.0) continue;
        // q = cross(s, edge1)
        const double qx = sy*e1z - sz*e1y;
        const double qy = sz*e1x - sx*e1z;
        const double qz = sx*e1y - sy*e1x;
        // v = f * dot(dir, q)
        const double v = f * (dir[0]*qx + dir[1]*qy + dir[2]*qz);
        if (v < 0.0 || u + v > 1.0) continue;
        // t = f * dot(edge2, q)
        const double t = f * (e2x*qx + e2y*qy + e2z*qz);
        if (t > kEps && t < best_t) best_t = t;
    }
    return best_t;
}

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

// [snippet:fk-write-omni-xform]
// Write a row-major 4x4 transform to `prim_path` via ovrtx_write_attribute
// using the same dtype / semantic as the camera write. Returns true on
// success, false on any error — does NOT print to stderr, so the caller
// can probe whether a prim exists (silent miss) and decide what to do.
//
// Used by the per-frame kinematic-arm FK driver to drive
// /World/KinematicArm/link_1 + link_2 from a sine wave. Same C API path
// the camera uses, just targeting `omni:xform` on a different prim.
bool try_write_omni_xform(ovrtx_renderer_t* renderer,
                          const char* prim_path,
                          std::array<double, 16>& m) {
    ovx_string_t p = {prim_path, std::strlen(prim_path)};
    ovrtx_prim_list_t list{};
    list.prim_paths = &p;
    list.num_paths = 1;

    ovrtx_attribute_type_t at{};
    at.dtype = {kDLFloat, 64, 16};       // mat4d packed into 16 lanes
    at.is_array = false;
    at.semantic = OVRTX_SEMANTIC_XFORM_MAT4x4;

    ovrtx_binding_desc_t bd{};
    bd.prim_list = list;
    bd.attribute_name.string = {"omni:xform", 10};
    bd.attribute_type = at;
    // EXISTING_ONLY ⇒ ovrtx returns an error instead of creating the
    // prim if it isn't already in the stage. That's exactly the probe
    // semantics we want: scenes without a KinematicArm (Test.usda,
    // 桂蘭樓_merge.usda) fail this call, we set fk_present = false,
    // and we never write again.
    bd.prim_mode = OVRTX_BINDING_PRIM_MODE_EXISTING_ONLY;
    bd.flags = OVRTX_BINDING_FLAG_NONE;

    ovrtx_binding_desc_or_handle_t boh{};
    boh.binding_desc = bd;

    DLTensor t{};
    t.data = m.data();
    t.device = {kDLCPU, 0};
    t.ndim = 1;
    int64_t shape[1] = {1};
    t.shape = shape;
    t.dtype = {kDLFloat, 64, 16};

    ovrtx_input_buffer_t buf{};
    buf.tensors = &t;
    buf.tensor_count = 1;

    auto wr = ovrtx_write_attribute(renderer, &boh, &buf,
                                    OVRTX_DATA_ACCESS_SYNC);
    return wr.status != OVRTX_API_ERROR;
}
// [/snippet:fk-write-omni-xform]

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
    // Compute the aggregate world-space bbox from the pick table so we
    // can reframe the orbit camera onto the actual scene center with a
    // sensible initial distance — instead of the heuristic 5/mpu in
    // main.cpp which fails for off-origin or unusually-sized assets.
    if (!pick_table_.empty()) {
        double scene_min[3] = { std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::infinity() };
        double scene_max[3] = { -std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity() };
        for (const auto& pe : pick_table_) {
            for (int i = 0; i < 3; ++i) {
                scene_min[i] = std::min(scene_min[i], pe.bb_min[i]);
                scene_max[i] = std::max(scene_max[i], pe.bb_max[i]);
            }
        }

        // Scene-bbox auto-frame: focus on center, distance ≈ 1.6× the
        // largest extent so the whole scene fits in a 60°-FOV viewport
        // with comfortable headroom. Zoom limits scale with the scene
        // so the wheel works for cm AGVs and meter buildings alike.
        const double cx = 0.5 * (scene_min[0] + scene_max[0]);
        const double cy = 0.5 * (scene_min[1] + scene_max[1]);
        const double cz = 0.5 * (scene_min[2] + scene_max[2]);
        const double ex = scene_max[0] - scene_min[0];
        const double ey = scene_max[1] - scene_min[1];
        const double ez = scene_max[2] - scene_min[2];
        const double max_extent = std::max({ex, ey, ez});
        const double init_distance = std::max(1e-3, 1.6 * max_extent);

        camera_.set_target(cx, cy, cz);
        camera_.set_distance(init_distance);
        camera_.set_distance_limits(init_distance * 0.01,
                                    init_distance * 50.0);
        std::fprintf(stderr,
            "[backend] scene bbox center=(%.2f, %.2f, %.2f) max_extent=%.2f "
            "→ camera distance=%.2f (zoom range %.2f..%.2f)\n",
            cx, cy, cz, max_extent,
            init_distance, init_distance * 0.01, init_distance * 50.0);
    }

    // pick_table_ is fully populated before the worker starts, so the
    // worker can read it lock-free for the rest of its lifetime.
    worker_ = std::thread(&AgvBackend::runWorker, this);
}

QString AgvBackend::lastPickedMaterial() const {
    std::lock_guard<std::mutex> g(picks_mutex_);
    return last_picked_material_;
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
    // ── Load the sidecar file ───────────────────────────────────────
    // The sidecar's stage header has `subLayers = [@./<scene>.usda@]`
    // which pulls in the source's entire top-level prim tree (so we
    // get /Environment with Test.usda's lights). The sidecar itself
    // declares /AgvCamera and /Render/OmniverseKit/HydraTextures/AgvViewport.
    ovrtx_usd_input_t usd_in{};
    usd_in.usd_file_path = {sidecar_path_.c_str(),
                            sidecar_path_.size()};
    ovrtx_usd_handle_t source_handle{};
    ovrtx_enqueue_result_t enq =
        ovrtx_add_usd(renderer, usd_in, {"", 0}, &source_handle);
    if (check_ovrtx(enq, "add_usd(sidecar)")) {
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
    std::fprintf(stderr, "[backend] sidecar loaded: %s\n",
                 sidecar_path_.c_str());

    // (The previous inline-USDA camera+render injection has moved into
    // the sidecar file. See sidecar.cpp::write_sidecar.)
#if 0
    // ── Inject camera + render product as a second inline sublayer ──
    //
    // Top-level Camera path (/AgvCamera) avoids composition surprises
    // with the source's /World prim and matches the placement of
    // Test.usda's own OmniverseKit_Persp camera.
    //
    // The apiSchemas on the Camera and RenderProduct are mandatory:
    // ovrtx's sensor framework only registers prims that declare these
    // schemas. Without them the renderer logs:
    //   "Coudn't find sensor prim associated with the render product"
    {
        // ovrtx's sensor framework only registers render products at the
        // hardcoded scope /Render/OmniverseKit/HydraTextures/<name>.
        // Compare planet-system/simple_scene.usda:268 — that's the
        // canonical location. Any product outside this scope is parsed
        // but never wired to the renderer ("Invalid USD RenderProduct").
        //
        // We also declare /Render/Vars/LdrColor here so the layer is
        // self-contained for scenes that don't author their own (e.g.
        // 桂蘭樓_merge.usda). When Test.usda is loaded it already has
        // an identical /Render/Vars/LdrColor — sublayer composition
        // merges the two declarations harmlessly.
        static const char* kCamRenderUsda = R"USDA(#usda 1.0
(
    defaultPrim = "AgvCamera"
)

def Camera "AgvCamera" (
    prepend apiSchemas = ["OmniRtxCameraAutoExposureAPI_1", "OmniRtxCameraExposureAPI_1"]
)
{
    token projection = "perspective"
    float focalLength = 18.5
    float horizontalAperture = 20.955
    float verticalAperture = 11.787
    float2 clippingRange = (0.1, 1e7)
    custom matrix4d omni:xform = (
        (1, 0, 0, 0),
        (0, 1, 0, 0),
        (0, 0, 1, 0),
        (0, 0, 4, 1)
    )
    uniform token[] xformOpOrder = ["omni:xform"]
    float exposure:fStop = 5
    float exposure:responsivity = 1
    float exposure:time = 0.02
}

def "Render"
{
    def "OmniverseKit"
    {
        def "HydraTextures"
        {
            def RenderProduct "AgvViewport" (
                prepend apiSchemas = ["OmniRtxSettingsCommonAdvancedAPI_1", "OmniRtxSettingsRtAdvancedAPI_1", "OmniRtxSettingsPtAdvancedAPI_1"]
            )
            {
                rel camera = </AgvCamera>
                rel orderedVars = </Render/Vars/LdrColor>
                uniform int2 resolution = (1920, 1080)
                custom token omni:rtx:rendermode = "RaytracedLighting"
                token omni:rtx:background:source:type = "domeLight"
            }
        }
    }

    def "Vars"
    {
        def RenderVar "LdrColor"
        {
            uniform string sourceName = "LdrColor"
            uniform token sourceType = "raw"
        }
    }
}
)USDA";
        ovrtx_usd_input_t cam_in{};
        cam_in.usd_layer_content = {kCamRenderUsda, std::strlen(kCamRenderUsda)};
        ovrtx_usd_handle_t cam_handle{};
        auto cam_enq = ovrtx_add_usd(renderer, cam_in, {"", 0}, &cam_handle);
        if (check_ovrtx(cam_enq, "add_usd(camera+render)")) {
            ovrtx_destroy_renderer(renderer); return;
        }
        while (ovrtx_wait_op(renderer, cam_enq.op_index,
                             ovrtx_timeout_t{0}, &wait).status
                == OVRTX_API_TIMEOUT) {
            if (stop_requested_.load()) {
                ovrtx_destroy_renderer(renderer); return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::fprintf(stderr, "[backend] camera + render product overlay loaded\n");
    }
#endif

    // (Environment overlay removed — Test.usda's CloudySky already
    // supplies DistantLight + DomeLight + SkyMaterial; injecting our
    // own /AgvEnvironment on top of it was double-counting and made
    // the scene over-bright. Scenes WITHOUT an authored sky — like
    // bare-bones 桂蘭樓_merge.usda — will look unlit. If you need a
    // fallback dome there, add a `--env` CLI flag that picks one of
    // a few preset USDA strings to inject, instead of always doing it.)

    // Camera-write descriptor — built once, reused every dirty frame.
    // Matches the top-level /AgvCamera path declared in the inline overlay.
    ovx_string_t cam_path = {"/AgvCamera", std::strlen("/AgvCamera")};
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

    // Render-product set we'll step. Path matches the canonical Kit-
    // sensor scope inside the inline overlay above
    // (/Render/OmniverseKit/HydraTextures/AgvViewport).
    static const char* kAgvProductPath =
        "/Render/OmniverseKit/HydraTextures/AgvViewport";
    ovx_string_t rp_str = {kAgvProductPath, std::strlen(kAgvProductPath)};
    ovrtx_render_product_set_t rp_set{};
    rp_set.render_products = &rp_str;
    rp_set.num_render_products = 1;

    // Material overrides own no ovrtx state of their own; bindings are
    // constructed inline per write. Cheap to keep local to the worker.
    MaterialOverrides materials(renderer);

    // [snippet:fk-probe]
    // Kinematic-arm FK driver. Probes once: if /World/KinematicArm/link_1
    // exists, we drive it (and link_2) every frame; otherwise we never
    // touch it again. Designed so the AGV scenes (no kinematic arm) get
    // zero overhead and zero stderr spam, while joint_example.usda picks
    // up live animation with no per-scene config flag.
    //
    // The probe write puts link_1 at its authored identity pose (worldZ
    // rotation = 0, world translate = (2, 0.5, 0)). On joint_example.usda
    // this is a no-op visually; on scenes without the prim it returns
    // false and we disable the driver.
    bool fk_present = false;
    {
        std::array<double, 16> identity_pose = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            2, 0.5, 0, 1
        };
        fk_present = try_write_omni_xform(
            renderer, "/World/KinematicArm/link_1", identity_pose);
        if (fk_present) {
            std::fprintf(stderr,
                "[backend] FK driver enabled "
                "(/World/KinematicArm/link_1 found)\n");
        } else {
            // Probe failed — clear the last-error slot so the next
            // legitimate failure isn't ambiguously reported.
            ovrtx_get_last_error();
            std::fprintf(stderr,
                "[backend] FK driver disabled "
                "(no KinematicArm in this scene)\n");
        }
    }
    const auto fk_start = std::chrono::steady_clock::now();
    // [/snippet:fk-probe]

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

                // Two-phase pick:
                //   1. AABB broadphase rejects meshes the ray doesn't
                //      enter at all (cheap, ~one cmp per axis per mesh).
                //   2. Ray-triangle narrowphase finds the actual closest
                //      visible surface for meshes whose bbox the ray
                //      did enter. This is what fixes the AGV's "always
                //      hits Base_Structure (the enveloping inner frame)"
                //      problem — the ray's AABB t for an enveloping box
                //      is small but its TRIANGLE t is far (because the
                //      ray exits the visible shell first).
                double best_t = std::numeric_limits<double>::infinity();
                const PickEntry* best = nullptr;
                // Per-pick diagnostics: count AABB passes and triangle
                // hits across all meshes. Prints only when no triangle
                // hit is found, so successful picks stay quiet.
                int aabb_hits = 0;
                int tri_hits  = 0;
                double sample_tbox_first = std::numeric_limits<double>::infinity();
                const PickEntry* sample_pe_first = nullptr;
                for (const auto& pe : pick_table_) {
                    const double t_box =
                        ray_aabb(eye, world_dir, pe.bb_min, pe.bb_max);
                    if (!std::isfinite(t_box) || t_box >= best_t) continue;
                    ++aabb_hits;
                    if (sample_pe_first == nullptr) {
                        sample_tbox_first = t_box;
                        sample_pe_first   = &pe;
                    }
                    // Bbox passes broadphase; do exact ray-triangle.
                    const double t_tri = ray_triangles_min_t(
                        eye, world_dir, pe.v0, pe.v1, pe.v2);
                    if (std::isfinite(t_tri)) ++tri_hits;
                    if (t_tri < best_t) {
                        best_t = t_tri;
                        best   = &pe;
                    }
                }
                if (best == nullptr) {
                    std::fprintf(stderr,
                        "[pick-debug] click=(%.3f, %.3f)  "
                        "eye=(%.2f, %.2f, %.2f)  "
                        "dir=(%.3f, %.3f, %.3f)  "
                        "aabb_hits=%d  tri_hits=%d  "
                        "first_aabb=%s t_box=%.2f tris=%zu\n",
                        x_frac, y_frac,
                        eye[0], eye[1], eye[2],
                        world_dir[0], world_dir[1], world_dir[2],
                        aabb_hits, tri_hits,
                        sample_pe_first ? sample_pe_first->mesh_path.c_str() : "(none)",
                        sample_tbox_first,
                        sample_pe_first ? sample_pe_first->v0.size() : 0);
                }
                if (best == nullptr ||
                    !std::isfinite(best_t)) {
                    std::fprintf(stderr,
                                 "[pick] miss at (%.3f, %.3f)\n",
                                 x_frac, y_frac);
                    continue;
                }
                std::fprintf(stderr,
                             "[pick] hit: %s  material=%s\n",
                             best->mesh_path.c_str(),
                             best->material_name.empty()
                                ? best->shader_path.c_str()
                                : best->material_name.c_str());
                {
                    std::lock_guard<std::mutex> g(picks_mutex_);
                    last_picked_material_ =
                        QString::fromStdString(best->material_name);
                }
                emit lastPickedMaterialChanged();
                materials.apply(*best, mode);
            }
        }

        // ── FK driver: animate /World/KinematicArm/link_1 + link_2 ─
        // Forward-kinematic 2-link arm:
        //   link_1 sits at world (2, 0.5, 0) and rotates around Z by a1.
        //   link_2 follows the tip of link_1 at length L along link_1's
        //   local +X axis, rotating Z by (a1 + a2) so the joint angle
        //   is purely relative.
        // Row-major layout matches USD's matrix4d (translation in row 3).
        // [snippet:fk-driver]
        if (fk_present) {
            double t = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - fk_start).count();
            double a1 = std::sin(t * 0.5) * 0.6;   // ~±34° at 0.5 rad/s
            double a2 = std::sin(t * 0.7) * 1.0;   // ~±57° at 0.7 rad/s
            const double L = 1.2;                  // link_1 → link_2 length

            double c1 = std::cos(a1), s1 = std::sin(a1);
            std::array<double, 16> m1 = {
                 c1, -s1, 0, 0,
                 s1,  c1, 0, 0,
                  0,   0, 1, 0,
                  2, 0.5, 0, 1
            };

            double a12 = a1 + a2;
            double c12 = std::cos(a12), s12 = std::sin(a12);
            // World position of link_2 = link_1_pos + Rz(a1) * (L, 0, 0)
            double tx2 = 2.0 + c1 * L;
            double ty2 = 0.5 + s1 * L;
            std::array<double, 16> m2 = {
                c12, -s12, 0, 0,
                s12,  c12, 0, 0,
                  0,    0, 1, 0,
                tx2,  ty2, 0, 1
            };

            try_write_omni_xform(renderer,
                                 "/World/KinematicArm/link_1", m1);
            try_write_omni_xform(renderer,
                                 "/World/KinematicArm/link_2", m2);
        }
        // [/snippet:fk-driver]

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
