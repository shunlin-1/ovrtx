// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// neon-robot-cef — ovrtx scene composited with a CEF HTML overlay.
//
// The render pipeline:
//   1. ovrtx steps the USD scene each frame and produces a CUDA array.
//   2. CUDA copies that array into a Vulkan-shared sampled image
//      (descriptor slot "scene_index").
//   3. CEF runs in off-screen rendering mode; OnPaint hands us a BGRA
//      bitmap that we upload to a second sampled image
//      (descriptor slot "ui_index").
//   4. A fullscreen.frag samples both, blurs the scene under glass
//      panels, and over-composites the premultiplied UI on top.
//
// Input routing:
//   - left-click  → CEF (so HTML buttons work)
//   - right-drag  → orbit camera (ovrtx scene)
//   - scroll      → zoom camera (ovrtx scene)
// Right-drag + scroll are not forwarded to CEF, so the UI doesn't
// fight the camera. left-button click+drag also feeds CEF so HTML
// drag interactions stay intact.

#include "camera/orbit_camera.hpp"
#include "cuda/cuda_kernel.hpp"
#include "glsl/spirv_loader.hpp"
#include "vk/vulkan_context.hpp"

#include <cuda.h>
#include <ovrtx/ovrtx.h>
#include <ovrtx/ovrtx_attributes.h>
#include <ovrtx/ovrtx_config.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/wrapper/cef_helpers.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <glm/gtc/type_ptr.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kWidth  = 1920;
constexpr int kHeight = 1080;

constexpr char const* DEFAULT_USD_FILE_URL        = "neon-only.usda";
constexpr char const* DEFAULT_RENDER_PRODUCT_PATH = "/Render/Camera";
constexpr UpAxis      DEFAULT_UP_AXIS             = UpAxis::Z;

// Neon ring animation (same constants as neon-robot-c).
constexpr int    NEON_NUM_LIGHTS       = 12;
constexpr double NEON_ORBIT_RADIUS     = 2.5;   // m
constexpr double NEON_ORBIT_HEIGHT     = 1.5;   // m
constexpr double NEON_BOB_AMPLITUDE    = 0.45;  // m
constexpr double NEON_ORBIT_PERIOD_SEC = 6.0;
constexpr double NEON_BOB_PERIOD_SEC   = 4.0;

enum class OutputType { HdrColor, LdrColor };

// ════════════════════════════════════════════════════════════════════
// CEF client — off-screen rendering, BGRA8 bitmap mirror.
// ════════════════════════════════════════════════════════════════════
class CefUiClient : public CefClient,
                    public CefRenderHandler,
                    public CefLifeSpanHandler,
                    public CefLoadHandler {
public:
    std::atomic<bool> got_paint{false};
    std::atomic<bool> page_loaded{false};
    std::atomic<int>  paint_count{0};
    std::atomic<bool> dirty{false};

    std::mutex            bmp_mutex;
    std::vector<uint8_t>  bmp;
    CefRefPtr<CefBrowser> browser;

    CefRefPtr<CefRenderHandler>   GetRenderHandler()   override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler>     GetLoadHandler()     override { return this; }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect = CefRect(0, 0, kWidth, kHeight);
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                 const RectList&, const void* buffer,
                 int width, int height) override {
        if (type != PET_VIEW) return;
        const auto bytes =
            static_cast<std::size_t>(width) * height * 4;
        {
            std::lock_guard<std::mutex> lock(bmp_mutex);
            bmp.assign(static_cast<const uint8_t*>(buffer),
                       static_cast<const uint8_t*>(buffer) + bytes);
        }
        ++paint_count;
        got_paint.store(true);
        dirty.store(true);
    }

    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                   int) override {
        if (frame->IsMain()) {
            page_loaded.store(true);
            std::fprintf(stderr, "[cef] page load end\n");
        }
    }
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                     ErrorCode err, const CefString& msg,
                     const CefString& failed_url) override {
        std::fprintf(stderr, "[cef] load error %d: %s (%s)\n",
                     err, msg.ToString().c_str(),
                     failed_url.ToString().c_str());
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> b) override {
        browser = b;
        std::fprintf(stderr, "[cef] browser created\n");
    }

    IMPLEMENT_REFCOUNTING(CefUiClient);
};

class CefHostApp : public CefApp {
public:
    IMPLEMENT_REFCOUNTING(CefHostApp);
};

// ════════════════════════════════════════════════════════════════════
// Globals for GLFW callbacks. The orbit-camera state is per-window;
// CEF input is forwarded through the same callbacks.
// ════════════════════════════════════════════════════════════════════
struct InputState {
    OrbitCamera* orbit_camera = nullptr;
    CefUiClient* cef          = nullptr;
    double       last_x       = 0.0;
    double       last_y       = 0.0;
    bool         right_pressed = false;
    bool         camera_dirty = true;
};
static InputState g_input;

// ════════════════════════════════════════════════════════════════════
// Forward declarations.
// ════════════════════════════════════════════════════════════════════
std::filesystem::path exe_dir();
std::string default_ui_url();
auto vulkan_format_for_output(OutputType type) -> VkFormat;
auto cuda_format_for_output(OutputType type) -> CudaImageFormat;
auto find_color_output(ovrtx_render_product_set_outputs_t const& outputs,
                       OutputType& output_type)
    -> ovrtx_rendered_output_handle_t;
template <typename ResultT>
bool check_and_print_error(ResultT const& result, std::string_view op);

void mouse_button_callback(GLFWwindow*, int, int, int);
void cursor_position_callback(GLFWwindow*, double, double);
void scroll_callback(GLFWwindow*, double, double);

ovrtx_enqueue_result_t update_neon_lights(
    ovrtx_renderer_t* renderer,
    std::vector<std::string> const& light_paths_str,
    double time_seconds);

// ════════════════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════════════════
int run(int argc, char* argv[]) {
    // ── Parse args ───────────────────────────────────────────────────
    std::string usd_file_path   = DEFAULT_USD_FILE_URL;
    std::string render_product  = DEFAULT_RENDER_PRODUCT_PATH;
    std::string ui_url;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--usd"  && i + 1 < argc) { usd_file_path = argv[++i]; }
        else if (a == "--url"  && i + 1 < argc) { ui_url        = argv[++i]; }
        else if (a == "--render-product" && i + 1 < argc) {
            render_product = argv[++i];
        }
    }
    if (ui_url.empty()) ui_url = default_ui_url();

    std::cerr << "[main] usd=" << usd_file_path
              << " render-product=" << render_product
              << " ui=" << ui_url << "\n";

    // ── ovrtx renderer + USD load ────────────────────────────────────
    std::cerr << "[ovrtx] init...\n";
    ovrtx_renderer_t* renderer = nullptr;
    ovrtx_config_t cfg = {};
    ovrtx_result_t rc = ovrtx_create_renderer(&cfg, &renderer);
    if (check_and_print_error(rc, "create_renderer")) return 1;

    ovrtx_usd_input_t usd_input = {};
    usd_input.usd_file_path.ptr    = usd_file_path.c_str();
    usd_input.usd_file_path.length = usd_file_path.size();
    ovx_string_t prefix = {"", 0};

    ovrtx_usd_handle_t usd_handle = 0;
    auto enq = ovrtx_add_usd(renderer, usd_input, prefix, &usd_handle);
    if (check_and_print_error(enq, "add_usd")) {
        ovrtx_destroy_renderer(renderer); return 1;
    }

    ovrtx_op_wait_result_t wait = {};
    rc = ovrtx_wait_op(renderer, enq.op_index,
                       ovrtx_timeout_infinite, &wait);
    if (wait.num_error_ops > 0) {
        for (int i = 0; i < wait.num_error_ops; ++i) {
            auto err = ovrtx_get_last_op_error(wait.error_op_ids[i]);
            std::cerr << "ERROR: "
                      << std::string_view(err.ptr, err.length) << "\n";
        }
        ovrtx_destroy_renderer(renderer); return 1;
    }
    if (check_and_print_error(rc, "wait_op(add_usd)")) {
        ovrtx_destroy_renderer(renderer); return 1;
    }
    std::cerr << "[ovrtx] USD loaded\n";

    // Light prim paths used by per-frame transform writes.
    std::vector<std::string> neon_light_paths(NEON_NUM_LIGHTS);
    for (int i = 0; i < NEON_NUM_LIGHTS; ++i) {
        neon_light_paths[i] = "/World/Lights/Neon_" + std::to_string(i);
    }
    auto neon_anim_start = std::chrono::steady_clock::now();

    // ── CUDA context (must match the Vulkan device for interop) ──────
    CUuuid cuda_uuid;
    if (!cuda_init(&cuda_uuid)) {
        std::cerr << "Failed to get CUDA context\n";
        ovrtx_destroy_renderer(renderer); return 1;
    }

    // ── Initial step to get render dimensions ────────────────────────
    ovx_string_t render_product_str = {
        render_product.c_str(),
        static_cast<int32_t>(render_product.size())};
    ovrtx_render_product_set_t render_products = {};
    render_products.render_products = &render_product_str;
    render_products.num_render_products = 1;

    ovrtx_step_result_handle_t step_result_handle = 0;
    enq = ovrtx_step(renderer, render_products, 0.0, &step_result_handle);
    if (check_and_print_error(enq, "step")) {
        ovrtx_destroy_renderer(renderer); return 1;
    }
    ovrtx_render_product_set_outputs_t outputs = {};
    rc = ovrtx_fetch_results(
        renderer, step_result_handle, ovrtx_timeout_infinite, &outputs);
    if (check_and_print_error(rc, "fetch_results")) {
        ovrtx_destroy_results(renderer, step_result_handle);
        ovrtx_destroy_renderer(renderer); return 1;
    }
    if (outputs.status != OVRTX_EVENT_COMPLETED || outputs.output_count == 0) {
        std::cerr << "No output for render product " << render_product << "\n";
        ovrtx_destroy_results(renderer, step_result_handle);
        ovrtx_destroy_renderer(renderer); return 3;
    }

    OutputType output_type;
    auto color_handle = find_color_output(outputs, output_type);
    if (color_handle == 0) {
        std::cerr << "No color output found\n";
        ovrtx_destroy_results(renderer, step_result_handle);
        ovrtx_destroy_renderer(renderer); return 1;
    }

    ovrtx_map_output_description_t map_desc = {};
    map_desc.device_type = OVRTX_MAP_DEVICE_TYPE_CUDA_ARRAY;
    map_desc.sync_stream = 0;

    ovrtx_rendered_output_t rendered_output = {};
    rc = ovrtx_map_rendered_output(renderer, color_handle, &map_desc,
                                   ovrtx_timeout_infinite, &rendered_output);
    if (check_and_print_error(rc, "map_rendered_output")) {
        ovrtx_destroy_results(renderer, step_result_handle);
        ovrtx_destroy_renderer(renderer); return 1;
    }
    int tex_w = static_cast<int>(rendered_output.buffer.dl.shape[1]);
    int tex_h = static_cast<int>(rendered_output.buffer.dl.shape[0]);
    ovrtx_unmap_rendered_output(renderer, rendered_output.map_handle,
                                ovrtx_cuda_sync_t{});
    ovrtx_destroy_results(renderer, step_result_handle);
    std::cerr << "[ovrtx] dims " << tex_w << "x" << tex_h
              << " type=" << (output_type == OutputType::HdrColor
                              ? "Hdr" : "Ldr") << "\n";

    // ── GLFW window ──────────────────────────────────────────────────
    if (!glfwInit()) {
        std::cerr << "glfwInit failed\n";
        ovrtx_destroy_renderer(renderer); return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(
        kWidth, kHeight, "neon-robot-cef", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        ovrtx_destroy_renderer(renderer); return 1;
    }

    // Orbit camera framed on the robot ring.
    float distance = 5.0f;
    OrbitCamera orbit_camera(distance,
                             glm::radians(290.0f),
                             std::asin(0.5f / 5.0f),
                             glm::vec3(0.0f, 0.0f, 1.0f),
                             DEFAULT_UP_AXIS);
    g_input.orbit_camera = &orbit_camera;
    g_input.camera_dirty = true;

    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);
    glfwSetScrollCallback(window, scroll_callback);

    int exit_code = 0;
    try {
        // ── Vulkan + shared images ──────────────────────────────────
        VulkanContextConfig vk_cfg;
        vk_cfg.window = window;
        vk_cfg.initial_sampled_image_capacity = 16;
        VulkanContext vk(vk_cfg, cuda_uuid);

        // ── Shaders ─────────────────────────────────────────────────
        auto vert_spirv = load_spirv(
            (exe_dir() / "shaders" / "fullscreen.vert.spv").string());
        auto frag_spirv = load_spirv(
            (exe_dir() / "shaders" / "fullscreen.frag.spv").string());
        auto [vert_shader, frag_shader] =
            vk.create_linked_vertex_and_fragment_shaders(vert_spirv,
                                                         frag_spirv);

        // ── ovrtx scene image: two-buffer ping-pong shared with CUDA ─
        constexpr int SHARED_IMAGE_COUNT = 2;
        VkFormat        scene_vk_fmt   = vulkan_format_for_output(output_type);
        CudaImageFormat scene_cuda_fmt = cuda_format_for_output(output_type);
        SampledImageHandle scene_images[SHARED_IMAGE_COUNT];
        for (int i = 0; i < SHARED_IMAGE_COUNT; ++i) {
            scene_images[i] = vk.create_sampled_image(
                tex_w, tex_h, scene_vk_fmt, VK_FILTER_LINEAR, true);
        }
        for (int i = 0; i < SHARED_IMAGE_COUNT; ++i) {
            VkImage img = vk.sampled_image(scene_images[i]).image;
            vk.immediate_submit([img](CommandBuffer cmd) {
                cmd.image_memory_barrier(
                    img, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    VK_ACCESS_2_NONE,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_WRITE_BIT);
            });
        }
        CUsurfObject cuda_surfaces[SHARED_IMAGE_COUNT];
        for (int i = 0; i < SHARED_IMAGE_COUNT; ++i) {
            auto const& img = vk.sampled_image(scene_images[i]);
            auto mem_handle = vk.export_memory_handle(scene_images[i]);
            cuda_surfaces[i] = cuda_import_vulkan_image(
                i, mem_handle, img.size, tex_w, tex_h, scene_cuda_fmt);
            if (cuda_surfaces[i] == 0) {
                throw std::runtime_error("cuda_import_vulkan_image failed");
            }
        }
        auto timeline_handle = vk.export_timeline_semaphore_handle();
        cuda_import_timeline_semaphore(timeline_handle);

        // ── CEF UI image: single non-shared sampled image + staging ─
        SampledImageHandle ui_handle = vk.create_sampled_image(
            kWidth, kHeight, VK_FORMAT_B8G8R8A8_UNORM,
            VK_FILTER_LINEAR, false);
        {
            VkImage img = vk.sampled_image(ui_handle).image;
            vk.immediate_submit([img](CommandBuffer cmd) {
                cmd.image_memory_barrier(
                    img, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    VK_ACCESS_2_NONE,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            });
        }

        constexpr VkDeviceSize UI_BYTES =
            VkDeviceSize(kWidth) * kHeight * 4;
        VkBuffer       staging        = VK_NULL_HANDLE;
        VkDeviceMemory staging_mem    = VK_NULL_HANDLE;
        void*          staging_mapped = nullptr;
        {
            VkBufferCreateInfo bci = {};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size  = UI_BYTES;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            vkCreateBuffer(vk.device(), &bci, nullptr, &staging);

            VkMemoryRequirements mreq;
            vkGetBufferMemoryRequirements(vk.device(), staging, &mreq);
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(vk.physical_device(), &mp);
            uint32_t mt = UINT32_MAX;
            const auto need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                if ((mreq.memoryTypeBits & (1u << i)) &&
                    (mp.memoryTypes[i].propertyFlags & need) == need) {
                    mt = i; break;
                }
            }
            VkMemoryAllocateInfo mai = {};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize  = mreq.size;
            mai.memoryTypeIndex = mt;
            vkAllocateMemory(vk.device(), &mai, nullptr, &staging_mem);
            vkBindBufferMemory(vk.device(), staging, staging_mem, 0);
            vkMapMemory(vk.device(), staging_mem, 0, UI_BYTES, 0,
                        &staging_mapped);
        }

        // ── CEF browser ─────────────────────────────────────────────
        CefRefPtr<CefUiClient> cef(new CefUiClient);
        g_input.cef = cef.get();
        {
            CefWindowInfo wi;
            wi.SetAsWindowless(0);
            CefBrowserSettings bs;
            bs.windowless_frame_rate = 60;
            CefBrowserHost::CreateBrowser(wi, cef.get(), ui_url, bs,
                                          nullptr, nullptr);
        }

        // ── CUDA stream / events ─────────────────────────────────────
        CUstream cuda_stream;
        cuStreamCreate(&cuda_stream, 0);
        CUevent cuda_start_event, cuda_end_event;
        CUevent cuda_frame_done_event, cuda_copy_done_event;
        cuEventCreate(&cuda_start_event,      CU_EVENT_DEFAULT);
        cuEventCreate(&cuda_end_event,        CU_EVENT_DEFAULT);
        cuEventCreate(&cuda_frame_done_event, CU_EVENT_DISABLE_TIMING);
        cuEventCreate(&cuda_copy_done_event,  CU_EVENT_DISABLE_TIMING);

        int      write_idx           = 0;
        int      read_idx            = 0;
        uint64_t cuda_frame_counter  = 0;
        uint64_t read_timeline_value = 0;
        bool     cuda_work_pending   = false;

        ovrtx_step_result_handle_t        current_step = 0;
        ovrtx_rendered_output_map_handle_t current_map  = 0;

        // ── Prime first ovrtx frame so Vulkan has valid content ─────
        {
            enq = ovrtx_step(renderer, render_products, 0.0, &current_step);
            if (check_and_print_error(enq, "step(prime)"))
                throw std::runtime_error("prime step failed");
            rc = ovrtx_fetch_results(renderer, current_step,
                                     ovrtx_timeout_infinite, &outputs);
            if (check_and_print_error(rc, "fetch_results(prime)"))
                throw std::runtime_error("prime fetch failed");
            OutputType ot;
            color_handle = find_color_output(outputs, ot);
            if (color_handle == OVRTX_INVALID_HANDLE)
                throw std::runtime_error("no color output in prime");
            rc = ovrtx_map_rendered_output(renderer, color_handle, &map_desc,
                                           ovrtx_timeout_infinite,
                                           &rendered_output);
            if (check_and_print_error(rc, "map(prime)"))
                throw std::runtime_error("prime map failed");
            current_map = rendered_output.map_handle;
            CUarray arr =
                reinterpret_cast<CUarray>(rendered_output.buffer.dl.data);
            CUevent wait_event = reinterpret_cast<CUevent>(
                rendered_output.buffer.cuda_sync.wait_event);
            int out_w = static_cast<int>(rendered_output.buffer.dl.shape[1]);
            int out_h = static_cast<int>(rendered_output.buffer.dl.shape[0]);
            if (wait_event) cuda_wait_event(wait_event, cuda_stream);
            cuda_copy_array_to_surface(0, arr, out_w, out_h,
                                       scene_cuda_fmt, cuda_stream);
            cuEventRecord(cuda_copy_done_event, cuda_stream);
            cuStreamSynchronize(cuda_stream);
            ovrtx_cuda_sync_t done_sync = {};
            done_sync.wait_event =
                reinterpret_cast<uintptr_t>(cuda_copy_done_event);
            ovrtx_unmap_rendered_output(renderer, current_map, done_sync);
            ovrtx_destroy_results(renderer, current_step);
            read_idx  = 0;
            write_idx = 1;
        }

        // ── Main loop ───────────────────────────────────────────────
        auto last_step_time = std::chrono::steady_clock::now();
        auto last_print     = std::chrono::steady_clock::now();
        auto start_time     = std::chrono::steady_clock::now();
        auto next_log       = start_time + std::chrono::seconds(10);
        int  frame_count    = 0;
        bool defer_swapchain_recreate = false;

        std::cerr << "[main] entering render loop\n";
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            // Pump CEF on the same thread we drive everything else.
            // OnPaint fires synchronously inside this call (because we
            // disable multi_threaded_message_loop), which is why we can
            // safely sample cef->dirty below.
            CefDoMessageLoopWork();

            auto size = vk.framebuffer_size();
            if (size.x == 0 || size.y == 0) {
                glfwWaitEvents();
                continue;
            }

            vk.wait_for_fence();
            if (defer_swapchain_recreate) {
                vk.recreate_swapchain();
                defer_swapchain_recreate = false;
                continue;
            }
            vk.reset_fence();

            uint32_t image_index = 0;
            auto acq = vk.acquire_next_image(image_index);
            if (acq == AcquireResult::OutOfDate) {
                vk.recreate_swapchain();
                vk.reset_fence_to_signaled();
                continue;
            } else if (acq == AcquireResult::Suboptimal) {
                defer_swapchain_recreate = true;
            } else if (acq == AcquireResult::Minimized) {
                vk.reset_fence_to_signaled();
                continue;
            }

            // ── Roll write/read indices once CUDA finishes ──────────
            if (cuda_work_pending) {
                if (cuEventQuery(cuda_frame_done_event) == CUDA_SUCCESS) {
                    read_timeline_value = cuda_frame_counter;
                    std::swap(read_idx, write_idx);
                    cuda_work_pending = false;
                }
            }

            // ── Push camera transform if it changed ─────────────────
            if (g_input.camera_dirty) {
                ovx_string_t cam_path = {"/World/Camera",
                                         (int32_t)strlen("/World/Camera")};
                ovrtx_prim_list_t pl = {};
                pl.prim_paths = &cam_path;
                pl.num_paths  = 1;
                ovrtx_attribute_type_t at = {};
                at.dtype.code = kDLFloat;
                at.dtype.bits = 64;
                at.dtype.lanes = 16;
                at.is_array = false;
                at.semantic = OVRTX_SEMANTIC_XFORM_MAT4x4;

                ovrtx_binding_desc_t bd = {};
                bd.prim_list = pl;
                char const* a_name = "omni:xform";
                bd.attribute_name.string.ptr    = a_name;
                bd.attribute_name.string.length = strlen(a_name);
                bd.attribute_type = at;
                bd.prim_mode      = OVRTX_BINDING_PRIM_MODE_EXISTING_ONLY;
                bd.flags          = OVRTX_BINDING_FLAG_NONE;

                ovrtx_binding_desc_or_handle_t bdh = {};
                bdh.binding_desc = bd;

                glm::mat4 xf = orbit_camera.transform_matrix();
                double xfd[16];
                float const* src = glm::value_ptr(xf);
                for (int i = 0; i < 16; ++i) xfd[i] = (double)src[i];

                DLTensor dl = {};
                dl.data = xfd;
                dl.device.device_type = kDLCPU;
                dl.ndim = 1;
                int64_t shape[1] = {1};
                dl.shape  = shape;
                dl.dtype.code  = kDLFloat;
                dl.dtype.bits  = 64;
                dl.dtype.lanes = 16;

                ovrtx_input_buffer_t ib = {};
                ib.tensors      = &dl;
                ib.tensor_count = 1;

                enq = ovrtx_write_attribute(renderer, &bdh, &ib,
                                            OVRTX_DATA_ACCESS_SYNC);
                if (check_and_print_error(enq, "write_attribute(camera)"))
                    throw std::runtime_error("camera write failed");
                g_input.camera_dirty = false;
            }

            // ── Enqueue next ovrtx step + CUDA copy (one in flight) ─
            if (!cuda_work_pending) {
                cuEventRecord(cuda_start_event, cuda_stream);
                auto now = std::chrono::steady_clock::now();
                double delta = std::chrono::duration<double>(
                    now - last_step_time).count();
                last_step_time = now;

                double neon_t = std::chrono::duration<double>(
                    now - neon_anim_start).count();
                auto neon_enq =
                    update_neon_lights(renderer, neon_light_paths, neon_t);
                if (check_and_print_error(neon_enq, "update_neon_lights"))
                    throw std::runtime_error("neon update failed");

                enq = ovrtx_step(renderer, render_products, delta,
                                 &current_step);
                if (check_and_print_error(enq, "step"))
                    throw std::runtime_error("step failed");
                rc = ovrtx_fetch_results(renderer, current_step,
                                         ovrtx_timeout_infinite, &outputs);
                if (check_and_print_error(rc, "fetch_results"))
                    throw std::runtime_error("fetch failed");
                OutputType ot;
                color_handle = find_color_output(outputs, ot);
                rc = ovrtx_map_rendered_output(renderer, color_handle,
                                               &map_desc,
                                               ovrtx_timeout_infinite,
                                               &rendered_output);
                if (check_and_print_error(rc, "map"))
                    throw std::runtime_error("map failed");
                current_map = rendered_output.map_handle;
                CUarray arr =
                    reinterpret_cast<CUarray>(rendered_output.buffer.dl.data);
                CUevent wait_ev = reinterpret_cast<CUevent>(
                    rendered_output.buffer.cuda_sync.wait_event);
                int ow = (int)rendered_output.buffer.dl.shape[1];
                int oh = (int)rendered_output.buffer.dl.shape[0];
                if (wait_ev) cuda_wait_event(wait_ev, cuda_stream);
                cuda_copy_array_to_surface(write_idx, arr, ow, oh,
                                           scene_cuda_fmt, cuda_stream);
                cuEventRecord(cuda_copy_done_event, cuda_stream);
                ovrtx_cuda_sync_t done = {};
                done.wait_event =
                    reinterpret_cast<uintptr_t>(cuda_copy_done_event);
                ovrtx_unmap_rendered_output(renderer, current_map, done);
                ovrtx_destroy_results(renderer, current_step);
                cuEventRecord(cuda_end_event,        cuda_stream);
                cuEventRecord(cuda_frame_done_event, cuda_stream);
                cuda_frame_counter++;
                cuda_signal_timeline(cuda_frame_counter, cuda_stream);
                cuda_work_pending = true;
            }

            // ── Record one frame: upload CEF, composite, present ────
            CommandBuffer cmd = vk.command_buffer();
            cmd.begin();

            VkImage read_image =
                vk.sampled_image(scene_images[read_idx]).image;
            cmd.image_memory_barrier(
                read_image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_QUEUE_FAMILY_EXTERNAL, vk.queue_family());

            // Upload latest CEF bitmap if dirty.
            if (cef->dirty.exchange(false)) {
                {
                    std::lock_guard<std::mutex> lock(cef->bmp_mutex);
                    if (!cef->bmp.empty()) {
                        std::memcpy(staging_mapped, cef->bmp.data(),
                                    std::min<std::size_t>(
                                        UI_BYTES, cef->bmp.size()));
                    }
                }
                VkImage ui_img = vk.sampled_image(ui_handle).image;
                cmd.image_memory_barrier(
                    ui_img, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkBufferImageCopy region = {};
                region.imageSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = {
                    uint32_t(kWidth), uint32_t(kHeight), 1};
                vkCmdCopyBufferToImage(cmd.handle(), staging, ui_img,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                cmd.image_memory_barrier(
                    ui_img, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }

            cmd.image_memory_barrier(
                vk.swapchain_image(image_index),
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_NONE,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            // ── Composite draw ──────────────────────────────────────
            VkRenderingAttachmentInfo ca = {};
            ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            ca.imageView = vk.swapchain_image_view(image_index);
            ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ca.clearValue.color = {{0.05f, 0.06f, 0.10f, 1.0f}};
            VkRenderingInfo ri = {};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea.offset = {0, 0};
            ri.renderArea.extent = vk.swapchain_extent();
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &ca;
            cmd.begin_rendering(ri);

            VkExtent2D ext = vk.swapchain_extent();
            cmd.set_viewport(0, 0, (float)ext.width, (float)ext.height);
            cmd.set_scissor(0, 0, ext.width, ext.height);
            cmd.set_rasterizer_discard_enable(false);
            cmd.set_polygon_mode(VK_POLYGON_MODE_FILL);
            cmd.set_cull_mode(VK_CULL_MODE_NONE);
            cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
            cmd.set_depth_bias_enable(false);
            cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            cmd.set_primitive_restart_enable(false);
            cmd.set_depth_test_enable(false);
            cmd.set_depth_write_enable(false);
            cmd.set_depth_bounds_test_enable(false);
            cmd.set_stencil_test_enable(false);
            cmd.set_rasterization_samples(VK_SAMPLE_COUNT_1_BIT);
            cmd.set_sample_mask(VK_SAMPLE_COUNT_1_BIT, 0xFFFFFFFF);
            cmd.set_alpha_to_coverage_enable(false);
            cmd.set_color_blend_enable(0, false);
            cmd.set_color_write_mask(0,
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
            cmd.set_vertex_input_empty();

            vk.bind_shaders(vert_shader, frag_shader);
            VkDescriptorSet desc = vk.descriptor_set();
            cmd.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     vk.pipeline_layout(), 0, 1, &desc);

            // The shader expects { scene_index, ui_index }. Until the
            // first CEF paint arrives, send UINT32_MAX for ui_index so
            // the fragment shader falls back to scene-only (its early
            // out at the top of fullscreen.frag).
            uint32_t indices[2] = {
                vk.sampled_image(scene_images[read_idx]).descriptor_index,
                cef->got_paint.load()
                    ? vk.sampled_image(ui_handle).descriptor_index
                    : 0xFFFFFFFFu,
            };
            cmd.push_constants(vk.pipeline_layout(),
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(indices), indices);
            cmd.draw(3);
            cmd.end_rendering();

            cmd.image_memory_barrier(
                vk.swapchain_image(image_index),
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                VK_ACCESS_2_NONE);

            cmd.image_memory_barrier(
                read_image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE,
                vk.queue_family(), VK_QUEUE_FAMILY_EXTERNAL);

            cmd.end();
            PresentResult pr = vk.submit_and_present(
                image_index, read_timeline_value);
            if (pr == PresentResult::OutOfDate ||
                pr == PresentResult::Suboptimal) {
                defer_swapchain_recreate = true;
            }

            frame_count++;
            auto now = std::chrono::steady_clock::now();
            if (now >= next_log) {
                auto secs = std::chrono::duration_cast<
                    std::chrono::seconds>(now - start_time).count();
                std::fprintf(stderr,
                    "[loop] uptime=%4llds frames=%d cef_paints=%d\n",
                    (long long)secs, frame_count,
                    cef->paint_count.load());
                next_log = now + std::chrono::seconds(10);
            }
            (void)last_print;
        }

        // ── Shutdown ────────────────────────────────────────────────
        cuStreamSynchronize(cuda_stream);
        vk.wait_for_fence();
        cuEventDestroy(cuda_start_event);
        cuEventDestroy(cuda_end_event);
        cuEventDestroy(cuda_frame_done_event);
        cuEventDestroy(cuda_copy_done_event);
        cuStreamDestroy(cuda_stream);
        cuda_cleanup();

        vkUnmapMemory(vk.device(), staging_mem);
        vkFreeMemory(vk.device(), staging_mem, nullptr);
        vkDestroyBuffer(vk.device(), staging, nullptr);

        if (cef->browser) cef->browser->GetHost()->CloseBrowser(true);
        for (int i = 0; i < 100; ++i) {
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } catch (std::exception const& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        exit_code = 1;
    }

    g_input.orbit_camera = nullptr;
    g_input.cef          = nullptr;
    glfwDestroyWindow(window);
    glfwTerminate();
    ovrtx_destroy_renderer(renderer);
    std::cerr << "Done.\n";
    return exit_code;
}

// ════════════════════════════════════════════════════════════════════
// CEF subprocess dispatch + entry point.
// CEF re-launches the same exe with --type= for renderer/GPU/utility
// subprocesses. CefExecuteProcess returns >=0 in those cases — we exit
// immediately without doing any window/Vulkan init. -1 means we're the
// main browser process.
// ════════════════════════════════════════════════════════════════════
int real_main(int argc, char* argv[]) {
#ifdef _WIN32
    CefMainArgs main_args(::GetModuleHandle(nullptr));
#else
    CefMainArgs main_args(argc, argv);
#endif
    CefRefPtr<CefHostApp> app(new CefHostApp);
    int proc_rc = CefExecuteProcess(main_args, app.get(), nullptr);
    if (proc_rc >= 0) return proc_rc;

    CefSettings settings;
    settings.no_sandbox                   = true;
    settings.windowless_rendering_enabled = true;
    settings.multi_threaded_message_loop  = false;
    settings.log_severity                 = LOGSEVERITY_WARNING;
    CefString(&settings.cache_path) =
        (std::filesystem::current_path() / "cef-cache").string();

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        std::fprintf(stderr, "CefInitialize failed\n");
        return 1;
    }
    int rc = 1;
    try {
        rc = run(argc, argv);
    } catch (std::exception const& e) {
        std::fprintf(stderr, "FATAL outer: %s\n", e.what());
    }
    CefShutdown();
    return rc;
}

// ════════════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════════════
std::filesystem::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return std::filesystem::current_path();
    return std::filesystem::path(buf).parent_path();
#else
    return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
}

std::string default_ui_url() {
    auto html = exe_dir() / "ui-html" / "index.html";
    if (!std::filesystem::exists(html)) {
        std::fprintf(stderr,
            "ui-html/index.html not found at %s\n",
            html.string().c_str());
        std::exit(1);
    }
    return "file:///" + html.generic_string();
}

auto vulkan_format_for_output(OutputType type) -> VkFormat {
    return (type == OutputType::HdrColor) ? VK_FORMAT_R16G16B16A16_SFLOAT
                                          : VK_FORMAT_R8G8B8A8_SRGB;
}
auto cuda_format_for_output(OutputType type) -> CudaImageFormat {
    return (type == OutputType::HdrColor) ? CudaImageFormat::Half4
                                          : CudaImageFormat::UInt8_4;
}

auto find_color_output(ovrtx_render_product_set_outputs_t const& outputs,
                       OutputType& output_type)
    -> ovrtx_rendered_output_handle_t {
    ovrtx_rendered_output_handle_t hdr = 0, ldr = 0;
    for (size_t i = 0; i < outputs.output_count; ++i) {
        auto const& po = outputs.outputs[i];
        for (size_t f = 0; f < po.output_frame_count; ++f) {
            auto const& fr = po.output_frames[f];
            for (size_t v = 0; v < fr.render_var_count; ++v) {
                auto const& var = fr.output_render_vars[v];
                if (!var.render_var_name.ptr) continue;
                if (strncmp(var.render_var_name.ptr, "HdrColor",
                            var.render_var_name.length) == 0)
                    hdr = var.output_handle;
                else if (strncmp(var.render_var_name.ptr, "LdrColor",
                                 var.render_var_name.length) == 0)
                    ldr = var.output_handle;
            }
        }
    }
    if (hdr) { output_type = OutputType::HdrColor; return hdr; }
    if (ldr) { output_type = OutputType::LdrColor; return ldr; }
    return 0;
}

template <typename ResultT>
bool check_and_print_error(ResultT const& result, std::string_view op) {
    if (result.status != OVRTX_API_SUCCESS) {
        auto err = ovrtx_get_last_error();
        if (err.ptr && err.length > 0) {
            std::cerr << "ovrtx " << op << " failed: "
                      << std::string_view(err.ptr, err.length) << "\n";
        } else {
            std::cerr << "ovrtx " << op << " failed\n";
        }
        return true;
    }
    return false;
}

ovrtx_enqueue_result_t update_neon_lights(
    ovrtx_renderer_t* renderer,
    std::vector<std::string> const& light_paths_str,
    double time_seconds) {
    int const n = (int)light_paths_str.size();
    std::vector<ovx_string_t> paths(n);
    std::vector<ovrtx_xform_matrix44d_t> xforms(n);

    double const orbit_speed = (2.0 * M_PI) / NEON_ORBIT_PERIOD_SEC;
    double const bob_speed   = (2.0 * M_PI) / NEON_BOB_PERIOD_SEC;

    for (int i = 0; i < n; ++i) {
        paths[i].ptr    = light_paths_str[i].c_str();
        paths[i].length = (int32_t)light_paths_str[i].size();

        double const phase     = (2.0 * M_PI * i) / n;
        double const angle     = time_seconds * orbit_speed + phase;
        double const bob_angle = time_seconds * bob_speed   + phase * 2.0;
        double const x = NEON_ORBIT_RADIUS * std::cos(angle);
        double const y = NEON_ORBIT_RADIUS * std::sin(angle);
        double const z = NEON_ORBIT_HEIGHT
                       + NEON_BOB_AMPLITUDE * std::sin(bob_angle);

        double* m = xforms[i].v;
        m[0]=1; m[1]=0; m[2]=0; m[3]=0;
        m[4]=0; m[5]=1; m[6]=0; m[7]=0;
        m[8]=0; m[9]=0; m[10]=1; m[11]=0;
        m[12]=x; m[13]=y; m[14]=z; m[15]=1;
    }
    return ovrtx_set_xform_mat(renderer, paths.data(), paths.size(),
                               xforms.data());
}

// ════════════════════════════════════════════════════════════════════
// GLFW callbacks: split input between CEF and the orbit camera.
//   - left mouse        → CEF only (so HTML buttons fire)
//   - right mouse drag  → camera orbit
//   - mouse move        → CEF (so hover effects work)
//   - scroll            → camera dolly (not forwarded to CEF)
// ════════════════════════════════════════════════════════════════════
static cef_mouse_button_type_t glfw_button_to_cef(int btn) {
    if (btn == GLFW_MOUSE_BUTTON_RIGHT)  return MBT_RIGHT;
    if (btn == GLFW_MOUSE_BUTTON_MIDDLE) return MBT_MIDDLE;
    return MBT_LEFT;
}

void mouse_button_callback(GLFWwindow* w, int button, int action, int) {
    int ww = 1, wh = 1; glfwGetWindowSize(w, &ww, &wh);
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        g_input.right_pressed = (action == GLFW_PRESS);
        return; // camera-only, do not forward
    }
    if (g_input.cef && g_input.cef->browser) {
        CefMouseEvent ev;
        ev.x = static_cast<int>(g_input.last_x * kWidth  / std::max(1, ww));
        ev.y = static_cast<int>(g_input.last_y * kHeight / std::max(1, wh));
        ev.modifiers = 0;
        g_input.cef->browser->GetHost()->SendMouseClickEvent(
            ev, glfw_button_to_cef(button),
            action == GLFW_RELEASE, 1);
    }
}

void cursor_position_callback(GLFWwindow* w, double xpos, double ypos) {
    int ww = 1, wh = 1; glfwGetWindowSize(w, &ww, &wh);
    if (g_input.right_pressed && g_input.orbit_camera) {
        float dx = static_cast<float>(xpos - g_input.last_x);
        float dy = static_cast<float>(ypos - g_input.last_y);
        g_input.orbit_camera->update(dx, dy);
        g_input.camera_dirty = true;
    } else if (g_input.cef && g_input.cef->browser) {
        CefMouseEvent ev;
        ev.x = static_cast<int>(xpos * kWidth  / std::max(1, ww));
        ev.y = static_cast<int>(ypos * kHeight / std::max(1, wh));
        ev.modifiers = 0;
        g_input.cef->browser->GetHost()->SendMouseMoveEvent(ev, false);
    }
    g_input.last_x = xpos;
    g_input.last_y = ypos;
}

void scroll_callback(GLFWwindow*, double, double yoffset) {
    if (!g_input.orbit_camera) return;
    constexpr float dolly = 0.1f;
    float d = g_input.orbit_camera->distance() *
              (1.0f - static_cast<float>(yoffset) * dolly);
    d = std::max(d, 0.1f);
    g_input.orbit_camera->set_distance(d);
    g_input.camera_dirty = true;
}

}  // namespace

int main(int argc, char* argv[]) {
    return real_main(argc, argv);
}
