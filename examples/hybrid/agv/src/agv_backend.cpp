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
#include <set>
#include <string_view>
#include <unordered_map>

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

// Y-clip predicate — given a mesh's AABB and the slider's Y, return
// `true` if the mesh should be HIDDEN. Tune to taste:
//
//   Option A — Hide stuff *entirely above* the cut (generous reveal,
//              meshes that straddle the cut stay visible):
//                  return entry.bb_min[1] > clip_y;
//
//   Option B (current) — Hide anything that *pokes above* the cut
//              (clean cross-section: only meshes entirely below the
//              cut survive, straddlers also hide). Closest we can get
//              to a horizontal "slice" without per-fragment clipping,
//              which ovrtx 0.2.0 doesn't expose.
//                  return entry.bb_max[1] > clip_y;
//
//   Option C — Hide by mesh centre (smooth fade across slider):
//                  return (entry.bb_min[1] + entry.bb_max[1]) * 0.5 > clip_y;
bool should_hide_at_y_clip(const PickEntry& entry, double clip_y) {
    // Option C — mesh CENTRE above cut. Smoothest transition because
    // centres are spread across the scene more evenly than extremes,
    // so the slider transitions feel gradual rather than stepped.
    return (entry.bb_min[1] + entry.bb_max[1]) * 0.5 > clip_y;
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
    // Compute the aggregate world-space bbox from the pick table so we
    // can (a) feed Y range to the Y-clip slider and (b) reframe the
    // orbit camera onto the actual scene center with a sensible initial
    // distance — instead of the heuristic 5/mpu in main.cpp which fails
    // for off-origin or unusually-sized assets.
    if (pick_table_.empty()) {
        y_clip_min_ = 0.0;
        y_clip_max_ = 1.0;
    } else {
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
        y_clip_min_ = scene_min[1];
        y_clip_max_ = scene_max[1];

        // Scene-bbox auto-frame: focus on center, distance ≈ 1.6× the
        // largest extent so the whole scene fits in a 60°-FOV viewport
        // with comfortable headroom. Zoom limits scale with the scene
        // so the slider works for cm AGVs and meter buildings alike.
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
    // Default Y-clip = top of scene → nothing clipped initially.
    y_clip_value_.store(y_clip_max_);

    // pick_table_ is fully populated before the worker starts, so the
    // worker can read it lock-free for the rest of its lifetime.
    worker_ = std::thread(&AgvBackend::runWorker, this);
}

QString AgvBackend::lastPickedMaterial() const {
    std::lock_guard<std::mutex> g(picks_mutex_);
    return last_picked_material_;
}

void AgvBackend::setBuildingXrayNeon(double v) {
    v = std::clamp(v, 0.0, 1.0);
    if (v == building_xray_neon_.load()) return;
    building_xray_neon_.store(v);
    xray_neon_dirty_.store(true);
    emit buildingXrayNeonChanged();
}

void AgvBackend::setYClipValue(double v) {
    if (v == y_clip_value_.load()) return;
    y_clip_value_.store(v);
    y_clip_dirty_.store(true);
    emit yClipValueChanged();
}

void AgvBackend::setSectionClipEnabled(bool enabled) {
    if (enabled == section_clip_enabled_.load()) return;
    section_clip_enabled_.store(enabled);
    section_clip_dirty_.store(true);
    // When entering section mode, also mark Y-clip dirty so the worker
    // writes the current slider value into cut_height_y immediately
    // (otherwise the clip material's default 1e6 leaves everything
    // visible until the slider next moves).
    y_clip_dirty_.store(true);
    emit sectionClipEnabledChanged();
    std::fprintf(stderr, "[backend] section clip -> %s\n",
                 enabled ? "ON" : "OFF");
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

    // ── Inject the environment as a runtime overlay layer ──────────
    // The current sidecar drops Test.usda's /Environment via the
    // defaultPrim filter, and 桂蘭樓_merge.usda has no environment at
    // all. We add a self-contained one here as inline USDA — no
    // on-disk file, no source-asset edits, NO network dependencies.
    //
    // Two lights:
    //   • DomeLight "Sky"  — flat sky-blue tint, IBL ambient
    //   • DistantLight "Sun" — directional shadows, sunlight tint
    //
    // To upgrade to a fancy HDR sky later, swap the DomeLight body for:
    //     def Xform "Sky" (
    //         prepend references = @https://omniverse-content-production.s3.us-west-2.amazonaws.com/Environments/2024_1/DomeLights/Dynamic/CumulusHeavy.usd@
    //     ) {}
    // — requires network access to NVIDIA's content S3 bucket on first run.
    {
        // Builds /AgvEnvironment (DomeLight + DistantLight) AND
        // /AgvLooks/SectionClip (custom MDL material for per-fragment
        // world-Y cross-section clipping). The .mdl asset path is
        // baked at compile time via AGV_MDL_DIR.
        std::string env_usda;
        env_usda.reserve(2048);
        env_usda += "#usda 1.0\n";
        env_usda += "(\n";
        env_usda += "    defaultPrim = \"AgvEnvironment\"\n";
        env_usda += ")\n\n";
        env_usda += "def Xform \"AgvEnvironment\"\n";
        env_usda += "{\n";
        env_usda += "    def DomeLight \"Sky\"\n";
        env_usda += "    {\n";
        env_usda += "        color3f inputs:color = (0.55, 0.70, 0.95)\n";
        env_usda += "        float inputs:intensity = 600\n";
        env_usda += "    }\n";
        env_usda += "    def DistantLight \"Sun\"\n";
        env_usda += "    {\n";
        env_usda += "        float inputs:angle = 0.53\n";
        env_usda += "        color3f inputs:color = (1.0, 0.97, 0.88)\n";
        env_usda += "        float inputs:intensity = 4\n";
        env_usda += "        float3 xformOp:rotateXYZ = (-45, 0, 30)\n";
        env_usda += "        uniform token[] xformOpOrder = [\"xformOp:rotateXYZ\"]\n";
        env_usda += "    }\n";
        env_usda += "}\n\n";
        // Custom MDL material for the per-fragment clipping mode.
        env_usda += "def \"AgvLooks\"\n";
        env_usda += "{\n";
        env_usda += "    def Material \"SectionClip\"\n";
        env_usda += "    {\n";
        env_usda += "        token outputs:mdl:surface.connect = "
                    "</AgvLooks/SectionClip/Shader.outputs:out>\n";
        env_usda += "        def Shader \"Shader\"\n";
        env_usda += "        {\n";
        env_usda += "            uniform token info:implementationSource = \"sourceAsset\"\n";
        env_usda += "            uniform asset info:mdl:sourceAsset = @";
        env_usda +=             AGV_MDL_DIR;
        env_usda +=             "/agv_section.mdl@\n";
        env_usda += "            uniform token info:mdl:sourceAsset:subIdentifier = \"AgvSection\"\n";
        env_usda += "            color3f inputs:section_color = (0.78, 0.85, 0.92)\n";
        env_usda += "            float  inputs:cut_height_y = 1000000.0\n";
        env_usda += "            color3f inputs:rim_color = (0.30, 0.85, 1.00)\n";
        env_usda += "            float  inputs:rim_intensity = 200.0\n";
        env_usda += "        }\n";
        env_usda += "    }\n";
        env_usda += "}\n";

        ovrtx_usd_input_t env_in{};
        env_in.usd_layer_content = {env_usda.c_str(), env_usda.size()};
        ovrtx_usd_handle_t env_handle{};
        auto env_enq = ovrtx_add_usd(renderer, env_in,
                                     {"", 0}, &env_handle);
        if (!check_ovrtx(env_enq, "add_usd(environment)")) {
            while (ovrtx_wait_op(renderer, env_enq.op_index,
                                 ovrtx_timeout_t{0}, &wait).status
                    == OVRTX_API_TIMEOUT) {
                if (stop_requested_.load()) {
                    ovrtx_destroy_renderer(renderer); return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::fprintf(stderr, "[backend] environment loaded (DomeLight + Sun)\n");
        }
    }

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

    // ── Pre-compute slider data ─────────────────────────────────────
    // Unique shader paths + parallel original color/intensity for the
    // X-ray Neon batched writes. Computed once; the data lives for the
    // worker's lifetime.
    std::vector<std::string>              unique_shader_paths;
    std::vector<std::array<float, 3>>     unique_orig_colors;
    std::vector<float>                    unique_orig_intensities;
    {
        std::unordered_map<std::string, std::size_t> seen;
        for (const auto& pe : pick_table_) {
            if (seen.emplace(pe.shader_path, unique_shader_paths.size()).second) {
                unique_shader_paths.push_back(pe.shader_path);
                unique_orig_colors.push_back(pe.orig_color);
                unique_orig_intensities.push_back(pe.orig_intensity);
            }
        }
        std::fprintf(stderr,
            "[backend] %zu meshes / %zu unique shaders -> slider targets %zu materials\n",
            pick_table_.size(), unique_shader_paths.size(),
            unique_shader_paths.size());
    }

    // Per-mesh Y-clip state. Tracks whether each mesh is currently hidden
    // so we only write deltas (avoid spamming ovrtx every slider tick).
    std::vector<bool> y_clip_hidden(pick_table_.size(), false);

    // Section-clip auxiliary tables: parallel arrays for the rebind
    // path. Original material path = parent of the shader_path (the
    // Material prim that owns the shader). Meshes whose shader_path
    // is empty (no MDL surface) are skipped from rebinding.
    std::vector<std::string> rebindable_mesh_paths;
    std::vector<std::string> original_material_paths;
    for (const auto& pe : pick_table_) {
        if (pe.shader_path.empty()) continue;
        const auto pos = pe.shader_path.find_last_of('/');
        if (pos == std::string::npos) continue;
        rebindable_mesh_paths.push_back(pe.mesh_path);
        original_material_paths.push_back(pe.shader_path.substr(0, pos));
    }
    std::fprintf(stderr,
        "[backend] %zu meshes eligible for section-clip rebind\n",
        rebindable_mesh_paths.size());

    static const std::string kSectionMaterialPath = "/AgvLooks/SectionClip";
    static const std::string kSectionShaderPath   =
        "/AgvLooks/SectionClip/Shader";

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

        // ── X-ray Neon slider: batched write to all unique shaders ──
        if (xray_neon_dirty_.exchange(false)) {
            double v = building_xray_neon_.load();
            apply_global_xray_neon(renderer,
                                   unique_shader_paths,
                                   unique_orig_colors,
                                   unique_orig_intensities,
                                   v);
        }

        // ── Section-clip toggle: rebind every mesh's material ───────
        if (section_clip_dirty_.exchange(false)) {
            const bool on = section_clip_enabled_.load();
            if (on) {
                // First, undo any per-mesh visibility hiding from the
                // alternate Y-clip mode so the section material has a
                // visible mesh to render on.
                std::vector<std::string> all_visible_paths;
                std::vector<bool>        all_visible_flag;
                for (std::size_t i = 0; i < pick_table_.size(); ++i) {
                    if (y_clip_hidden[i]) {
                        all_visible_paths.push_back(pick_table_[i].mesh_path);
                        all_visible_flag.push_back(false);  // -> inherited
                        y_clip_hidden[i] = false;
                    }
                }
                if (!all_visible_paths.empty()) {
                    apply_visibility(renderer, all_visible_paths,
                                     all_visible_flag);
                }
                apply_material_rebind(renderer, rebindable_mesh_paths,
                                      kSectionMaterialPath);
            } else {
                apply_material_restore(renderer, rebindable_mesh_paths,
                                       original_material_paths);
            }
        }

        // ── Y-clip slider: mode-dispatched ──────────────────────────
        //   Section clip ON  → write cut_height_y to the clip Shader.
        //   Section clip OFF → toggle per-mesh visibility (old path).
        if (y_clip_dirty_.exchange(false)) {
            const double clip_y = y_clip_value_.load();
            if (section_clip_enabled_.load()) {
                float ch = static_cast<float>(clip_y);
                // Direct attribute write — one ovrtx call per slider tick.
                ovx_string_t pp = {kSectionShaderPath.c_str(),
                                   kSectionShaderPath.size()};
                ovrtx_prim_list_t pl{};
                pl.prim_paths = &pp; pl.num_paths = 1;
                ovrtx_attribute_type_t at{};
                at.dtype = {kDLFloat, 32, 1};
                at.is_array = false;
                ovrtx_binding_desc_t b{};
                b.prim_list = pl;
                b.attribute_name.string = {"inputs:cut_height_y", 19};
                b.attribute_type = at;
                b.prim_mode = OVRTX_BINDING_PRIM_MODE_EXISTING_ONLY;
                b.flags = OVRTX_BINDING_FLAG_NONE;
                ovrtx_binding_desc_or_handle_t boh{};
                boh.binding_desc = b;
                DLTensor t{};
                t.data = &ch;
                t.device = {kDLCPU, 0};
                t.ndim = 1;
                int64_t shape[1] = {1};
                t.shape = shape;
                t.dtype = {kDLFloat, 32, 1};
                ovrtx_input_buffer_t in_buf{};
                in_buf.tensors = &t;
                in_buf.tensor_count = 1;
                ovrtx_write_attribute(renderer, &boh, &in_buf,
                                      OVRTX_DATA_ACCESS_SYNC);
            } else {
                std::vector<std::string> changed_paths;
                std::vector<bool>        changed_hide;
                for (std::size_t i = 0; i < pick_table_.size(); ++i) {
                    const bool want_hidden =
                        should_hide_at_y_clip(pick_table_[i], clip_y);
                    if (want_hidden != y_clip_hidden[i]) {
                        changed_paths.push_back(pick_table_[i].mesh_path);
                        changed_hide.push_back(want_hidden);
                        y_clip_hidden[i] = want_hidden;
                    }
                }
                if (!changed_paths.empty()) {
                    apply_visibility(renderer, changed_paths, changed_hide);
                }
            }
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
