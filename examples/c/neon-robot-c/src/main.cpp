// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
// property and proprietary rights in and to this material, related
// documentation and any modifications thereto. Any use, reproduction,
// disclosure or distribution of this material and related documentation
// without an express license agreement from NVIDIA CORPORATION or
// its affiliates is strictly prohibited.

#include "camera/orbit_camera.hpp"
#include "cuda/cuda_kernel.hpp"
#include "glsl/spirv_loader.hpp"
#include "vk/vulkan_context.hpp"
#include <cuda.h>
#include <ovrtx/ovrtx.h>
#include <ovrtx/ovrtx_attributes.h>
#include <ovrtx/ovrtx_config.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <glm/gtc/type_ptr.hpp>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Window dimensions
constexpr int WINDOW_WIDTH = 1920;
constexpr int WINDOW_HEIGHT = 1080;

// Mouse state for orbit camera
static bool g_mouse_pressed = false;
static double g_last_mouse_x = 0.0;
static double g_last_mouse_y = 0.0;
static bool g_camera_dirty = false;
static OrbitCamera* g_orbit_camera = nullptr;

// Scene units for distance scaling
enum class Units { Centimeters, Meters };

// Default scene configuration. Points at the bundled neon-only.usda
// (resolved relative to cwd) — a self-contained scene with a ground plane,
// camera, render product, dome fill light, and six SphereLights that this
// binary animates each frame. No external dependency on the robot scene.
static constexpr char const* DEFAULT_USD_FILE_URL = "neon-only.usda";
static constexpr char const* DEFAULT_RENDER_PRODUCT_PATH = "/Render/Camera";
static constexpr UpAxis DEFAULT_UP_AXIS = UpAxis::Z;
static constexpr Units DEFAULT_SCENE_UNITS = Units::Meters;

static auto get_executable_dir() -> std::filesystem::path;
static void
mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
static void
cursor_position_callback(GLFWwindow* window, double xpos, double ypos);
static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
static void print_usage(char const* program_name);
static bool parse_args(int argc,
                       char* argv[],
                       std::string& usd_file,
                       std::string& render_product,
                       UpAxis& up_axis,
                       Units& units,
                       int& num_frames);
template <typename ResultT>
static bool check_and_print_error(ResultT const& result,
                                  std::string_view operation);

// Output type determines pixel format for CUDA-Vulkan interop
enum class OutputType { HdrColor, LdrColor };

static auto vulkan_format_for_output(OutputType type) -> VkFormat;
static auto cuda_format_for_output(OutputType type) -> CudaImageFormat;
static auto output_type_name(OutputType type) -> char const*;
static void print_frame_time_stats(int total_frames,
                                   std::vector<double> const& cpu_times_ms,
                                   std::vector<double> const& vulkan_times_ms,
                                   std::vector<double> const& cuda_times_ms);
static void record_rendering_state(CommandBuffer& cmd,
                                   VulkanContext& vk,
                                   uint32_t swapchain_image_index,
                                   ShaderHandle vert_shader,
                                   ShaderHandle frag_shader,
                                   SampledImageHandle read_image_handle);

// Search for color output in ovrtx results, preferring HdrColor over LdrColor
// Returns the output handle and sets the output_type
static auto find_color_output(ovrtx_render_product_set_outputs_t const& outputs,
                              OutputType& output_type)
    -> ovrtx_render_var_output_handle_t;

// =========================================================================
// Neon ring animation: 6 SphereLights orbit at constant chest height around
// the origin. The lights themselves are authored statically in neon-only.usda
// (with their initial positions, neon colors, and intensities); this code
// rewrites their omni:xform every frame to animate the ring.
//
// Scene units are meters, Z is up.
// =========================================================================
static constexpr int NEON_NUM_LIGHTS = 12;
static constexpr double NEON_ORBIT_RADIUS = 2.5;       // m
static constexpr double NEON_ORBIT_HEIGHT = 1.5;       // m, ring centre on Z
static constexpr double NEON_BOB_AMPLITUDE = 0.45;     // m peak Z bob
static constexpr double NEON_ORBIT_PERIOD_SEC = 6.0;   // one full revolution
static constexpr double NEON_BOB_PERIOD_SEC = 4.0;     // vertical wave period

// [snippet:update-neon-lights]
// Recompute orbital positions for each light at `time_seconds` and push the
// new transforms via the synchronous helper. The lights already exist in the
// loaded USDA at /World/Lights/Neon_<i>; we only mutate their omni:xform.
static ovrtx_enqueue_result_t
update_neon_lights(ovrtx_renderer_t* renderer,
                   std::vector<std::string> const& light_paths_str,
                   double time_seconds) {
    int const n = static_cast<int>(light_paths_str.size());
    std::vector<ovx_string_t> paths(n);
    std::vector<ovrtx_xform_matrix44d_t> xforms(n);

    double const orbit_speed = (2.0 * M_PI) / NEON_ORBIT_PERIOD_SEC;
    double const bob_speed = (2.0 * M_PI) / NEON_BOB_PERIOD_SEC;

    for (int i = 0; i < n; ++i) {
        paths[i].ptr = light_paths_str[i].c_str();
        paths[i].length = light_paths_str[i].size();

        double const phase = (2.0 * M_PI * i) / n;
        double const angle = time_seconds * orbit_speed + phase;
        // Bob has its own phase offset so neighbouring lights wave at
        // different heights as they orbit -- "Mexican wave" pattern.
        double const bob_angle = time_seconds * bob_speed + phase * 2.0;

        // Z-up: ring lives in XY plane, height varies sinusoidally on Z.
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
// [/snippet:update-neon-lights]


int main(int argc, char* argv[]) {
    // Parse command-line arguments
    std::string usd_file_path;
    std::string render_product_path;
    UpAxis up_axis;
    Units units;
    int num_frames = 0;

    if (!parse_args(argc,
                    argv,
                    usd_file_path,
                    render_product_path,
                    up_axis,
                    units,
                    num_frames)) {
        return 0; // Help was shown or parse error
    }

    std::cerr << "USD file: " << usd_file_path << "\n";
    std::cerr << "Render product: " << render_product_path << "\n";
    std::cerr << "Up axis: " << (up_axis == UpAxis::Y ? "Y" : "Z") << "\n";
    std::cerr << "Units: "
              << (units == Units::Meters ? "meters" : "centimeters") << "\n";

    // =========================================================================
    // Initialize ovrtx
    // =========================================================================
    std::cerr << "Initializing ovrtx...\n";

    ovrtx_renderer_t* renderer = nullptr;
    GLFWwindow* window = nullptr;

    // ovrtx owns scene evaluation and rendering; the rest of this sample
    // bridges those results into Vulkan-presentable images.
    // [snippet:initialize-and-create-renderer]
    ovrtx_config_t ovrtx_config = {};
    ovrtx_result_t result = ovrtx_create_renderer(&ovrtx_config, &renderer);
    if (check_and_print_error(result, "create_renderer")) {
        return 1;
    }
    // [/snippet:initialize-and-create-renderer]

    std::cerr << "Loading USD scene: " << usd_file_path << "\n";

    // Load USD file. 0.3 split ovrtx_add_usd() into root-layer vs. reference
    // APIs; this is the scene's root layer, so it becomes open_usd_from_file.
    ovx_string_t usd_file = {};
    usd_file.ptr = usd_file_path.c_str();
    usd_file.length = usd_file_path.size();

    // Loading USD is asynchronous in ovrtx, so we enqueue then wait for
    // completion.
    ovrtx_enqueue_result_t enqueue_result =
        ovrtx_open_usd_from_file(renderer, usd_file);
    if (check_and_print_error(enqueue_result, "open_usd_from_file")) {
        ovrtx_destroy_renderer(renderer);
        return 1;
    }

    ovrtx_op_wait_result_t wait_result = {};
    result = ovrtx_wait_op(renderer,
                           enqueue_result.op_index,
                           ovrtx_timeout_infinite,
                           &wait_result);
    if (wait_result.num_error_ops > 0) {
        for (int i = 0; i < wait_result.num_error_ops; ++i) {
            ovx_string_t error_string =
                ovrtx_get_last_op_error(wait_result.error_op_ids[i]);
            std::cerr << "ERROR: "
                      << std::string_view(error_string.ptr, error_string.length)
                      << std::endl;
        }

        ovrtx_destroy_renderer(renderer);
        return 1;
    }
    if (check_and_print_error(result, "wait_op (add_usd)")) {
        ovrtx_destroy_renderer(renderer);
        return 1;
    }

    std::cerr << "USD scene loaded successfully\n";
    std::cerr << "Render product path: " << render_product_path << "\n";

    // Light prim paths in the loaded scene; used by per-frame transform writes.
    std::vector<std::string> neon_light_paths(NEON_NUM_LIGHTS);
    for (int i = 0; i < NEON_NUM_LIGHTS; ++i) {
        neon_light_paths[i] = "/World/Lights/Neon_" + std::to_string(i);
    }
    auto neon_anim_start = std::chrono::steady_clock::now();

    // ovrtx creates/uses CUDA internally; we query that context so Vulkan can
    // be created on the exact same physical device for interop safety.
    CUuuid cuda_uuid;
    if (!cuda_init(&cuda_uuid)) {
        std::cerr << "Failed to get CUDA context\n";
        ovrtx_destroy_renderer(renderer);
        return 1;
    }

    // =========================================================================
    // Do an initial ovrtx step to get render dimensions
    // =========================================================================
    // We need output dimensions and format before allocating shared Vulkan
    // images.
    ovx_string_t render_product_str = {};
    render_product_str.ptr = render_product_path.c_str();
    render_product_str.length = render_product_path.size();

    ovrtx_render_product_set_t render_products = {};
    render_products.render_products = &render_product_str;
    render_products.num_render_products = 1;

    ovrtx_step_result_handle_t step_result_handle = 0;
    enqueue_result =
        ovrtx_step(renderer, render_products, 0.0, &step_result_handle);
    if (check_and_print_error(enqueue_result, "step")) {
        ovrtx_destroy_renderer(renderer);
        return 1;
    }

    ovrtx_render_product_set_outputs_t outputs = {};
    result = ovrtx_fetch_results(
        renderer, step_result_handle, ovrtx_timeout_infinite, &outputs);
    if (check_and_print_error(result, "fetch_results")) {
        ovrtx_destroy_results(renderer, step_result_handle);
        ovrtx_destroy_renderer(renderer);
        return 1;
    }

    if (outputs.status != OVRTX_EVENT_COMPLETED || outputs.output_count == 0) {
        std::cerr << "Could not get dimensions for RenderProduct \""
                  << render_product_path << "\"" << std::endl;
        ovrtx_destroy_results(renderer, step_result_handle);
        ovrtx_destroy_renderer(renderer);
        return 3;
    }

    // Find color output (prefer HdrColor, fall back to LdrColor)
    OutputType output_type;
    ovrtx_render_var_output_handle_t color_output_handle =
        find_color_output(outputs, output_type);
    if (color_output_handle == 0) {
        std::cerr << "No color output found (HdrColor or LdrColor)\n";
        ovrtx_destroy_results(renderer, step_result_handle);
        ovrtx_destroy_renderer(renderer);
        return 1;
    }
    std::cerr << "Using " << output_type_name(output_type) << " output\n";

    // Request CUDA-array mapping so ovrtx gives us a GPU-native source buffer
    // that can be copied directly into exported Vulkan memory.
    ovrtx_map_output_description_t map_desc = {};
    map_desc.device_type = OVRTX_MAP_DEVICE_TYPE_CUDA_ARRAY;
    map_desc.sync_stream = 0;

    ovrtx_render_var_output_t rendered_output = {};
    result = ovrtx_map_render_var_output(renderer,
                                       color_output_handle,
                                       &map_desc,
                                       ovrtx_timeout_infinite,
                                       &rendered_output);
    if (check_and_print_error(result, "map_render_var_output")) {
        ovrtx_destroy_results(renderer, step_result_handle);
        ovrtx_destroy_renderer(renderer);
        return 1;
    }

    // 0.3 replaced the single `buffer` field with a tensors[] array so
    // composite render vars can carry several named tensors. Color outputs
    // are single-tensor.
    if (rendered_output.num_tensors != 1) {
        std::fprintf(stderr,
                     "Expected 1 tensor for the color output, got %zu\n",
                     rendered_output.num_tensors);
        ovrtx_unmap_render_var_output(
            renderer, rendered_output.map_handle, ovrtx_cuda_sync_t{});
        ovrtx_destroy_results(renderer, step_result_handle);
        ovrtx_destroy_renderer(renderer);
        return 1;
    }
    DLTensor const& dl = (*rendered_output.tensors[0].dl);
    int tex_width = static_cast<int>(dl.shape[1]);
    int tex_height = static_cast<int>(dl.shape[0]);

    // Unmap and destroy initial step results (no copy, so no sync needed)
    result = ovrtx_unmap_render_var_output(
        renderer, rendered_output.map_handle, ovrtx_cuda_sync_t{});
    if (check_and_print_error(result, "unmap_rendered_output")) {
        ovrtx_destroy_results(renderer, step_result_handle);
        ovrtx_destroy_renderer(renderer);
        return 1;
    }
    result = ovrtx_destroy_results(renderer, step_result_handle);
    if (check_and_print_error(result, "destroy_results")) {
        ovrtx_destroy_renderer(renderer);
        return 1;
    }

    std::cerr << "Render output dimensions: " << tex_width << " x "
              << tex_height << "\n";

    // =========================================================================
    // Initialize GLFW and create window
    // =========================================================================
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        ovrtx_destroy_renderer(renderer);
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window = glfwCreateWindow(
        WINDOW_WIDTH, WINDOW_HEIGHT, "ovrtx-Vulkan Interop", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create window\n";
        glfwTerminate();
        ovrtx_destroy_renderer(renderer);
        return 1;
    }

    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);
    glfwSetScrollCallback(window, scroll_callback);

    // Create orbit camera
    // Start with a decent framing on the robot
    // Scale camera distance based on scene units (centimeters need 100x scale)
    float unit_scale = (units == Units::Centimeters) ? 100.0f : 1.0f;
    float distance = 5.0f * unit_scale; // 5 meters or 500 centimeters
    float azimuth = glm::radians(290.0f);
    float elevation =
        std::asin(0.5f / 5.0f);         // camera at ~1.5m looking at 1m target
    glm::vec3 target(0.0f, 0.0f, 1.0f); // 1 meter above ground (Z-up)
    OrbitCamera orbit_camera(distance, azimuth, elevation, target, up_axis);
    g_orbit_camera = &orbit_camera;
    g_camera_dirty = true;

    // Scope for VulkanContext
    try {
        // VulkanContext chooses a Vulkan device that matches the CUDA UUID
        // above.
        VulkanContextConfig vk_config;
        vk_config.window = window;
        vk_config.initial_sampled_image_capacity = 16;

        VulkanContext vk(vk_config, cuda_uuid);

        // Two shared images enable ping-pong: CUDA writes one while Vulkan
        // samples the other, avoiding read/write hazards within a frame.
        constexpr int SHARED_IMAGE_COUNT = 2;
        VkFormat vk_format = vulkan_format_for_output(output_type);
        CudaImageFormat cuda_format = cuda_format_for_output(output_type);

        SampledImageHandle shared_images[SHARED_IMAGE_COUNT];
        for (int i = 0; i < SHARED_IMAGE_COUNT; ++i) {
            shared_images[i] = vk.create_sampled_image(
                tex_width, tex_height, vk_format, VK_FILTER_LINEAR, true);
        }

        // Load precompiled SPIR-V shaders from next to the executable
        std::filesystem::path shader_dir = get_executable_dir() / "shaders";
        std::string vert_path = (shader_dir / "fullscreen.vert.spv").string();
        std::string frag_path = (shader_dir / "fullscreen.frag.spv").string();
        std::vector<uint32_t> vert_spirv = load_spirv(vert_path);
        std::vector<uint32_t> frag_spirv = load_spirv(frag_path);

        std::cerr << "Loaded shaders: vertex=" << vert_spirv.size()
                  << " words, fragment=" << frag_spirv.size() << " words\n";

        auto [vert_shader, frag_shader] =
            vk.create_linked_vertex_and_fragment_shaders(vert_spirv,
                                                         frag_spirv);

        // CUDA writes require GENERAL layout and external queue ownership.
        for (int i = 0; i < SHARED_IMAGE_COUNT; ++i) {
            VkImage shared_img = vk.sampled_image(shared_images[i]).image;
            vk.immediate_submit([shared_img](CommandBuffer cmd) {
                cmd.image_memory_barrier(shared_img,
                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                         VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                         VK_ACCESS_2_NONE,
                                         VK_IMAGE_LAYOUT_GENERAL,
                                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                         VK_ACCESS_2_SHADER_WRITE_BIT);
            });
        }

        // Export VkDeviceMemory and import into CUDA as surface objects, which
        // become copy targets for mapped ovrtx CUDA arrays.
        CUsurfObject cuda_surfaces[SHARED_IMAGE_COUNT];
        for (int i = 0; i < SHARED_IMAGE_COUNT; ++i) {
            SampledImage const& img = vk.sampled_image(shared_images[i]);
            auto memory_handle = vk.export_memory_handle(shared_images[i]);
            cuda_surfaces[i] = cuda_import_vulkan_image(
                i, memory_handle, img.size, tex_width, tex_height, cuda_format);
            if (cuda_surfaces[i] == 0) {
                std::cerr << "Failed to import Vulkan image " << i
                          << " into CUDA\n";
                cuda_cleanup();
                glfwDestroyWindow(window);
                glfwTerminate();
                ovrtx_destroy_renderer(renderer);
                return 1;
            }
        }

        // Timeline semaphore synchronizes async CUDA producers with Vulkan
        // consumers without forcing CPU-side blocking.
        auto timeline_handle = vk.export_timeline_semaphore_handle();
        cuda_import_timeline_semaphore(timeline_handle);

        // CUDA events track copy completion and provide fence points back to
        // ovrtx.
        CUstream cuda_stream;
        cuStreamCreate(&cuda_stream, 0);

        CUevent cuda_start_event, cuda_end_event, cuda_frame_done_event,
            cuda_copy_done_event;
        cuEventCreate(&cuda_start_event, CU_EVENT_DEFAULT);
        cuEventCreate(&cuda_end_event, CU_EVENT_DEFAULT);
        cuEventCreate(&cuda_frame_done_event, CU_EVENT_DISABLE_TIMING);
        cuEventCreate(&cuda_copy_done_event, CU_EVENT_DISABLE_TIMING);

        // Double-buffer state: write_idx is CUDA's target, read_idx is Vulkan's
        // source.
        int write_idx = 0;
        int read_idx = 0;
        uint64_t cuda_frame_counter = 0;
        uint64_t read_timeline_value =
            0; // timeline value for current read buffer
        bool cuda_work_pending = false;

        // ovrtx step state
        ovrtx_step_result_handle_t current_step_result = 0;
        ovrtx_render_var_output_map_handle_t current_map_handle = 0;
        bool has_mapped_output = false;

        // =====================================================================
        // Prime the first buffer so Vulkan has valid content before
        // steady-state async producer/consumer overlap begins.
        // =====================================================================
        std::cerr << "Priming first frame...\n";
        {
            enqueue_result = ovrtx_step(
                renderer, render_products, 0.0, &current_step_result);
            if (check_and_print_error(enqueue_result, "step")) {
                cuda_cleanup();
                glfwDestroyWindow(window);
                glfwTerminate();
                ovrtx_destroy_renderer(renderer);
                return 1;
            }

            result = ovrtx_fetch_results(renderer,
                                         current_step_result,
                                         ovrtx_timeout_infinite,
                                         &outputs);
            if (check_and_print_error(result, "fetch_results")) {
                cuda_cleanup();
                glfwDestroyWindow(window);
                glfwTerminate();
                ovrtx_destroy_renderer(renderer);
                return 1;
            }

            // Find color output (same type as detected initially)
            OutputType frame_output_type;
            color_output_handle = find_color_output(outputs, frame_output_type);
            if (color_output_handle == OVRTX_INVALID_HANDLE) {
                std::cerr << "ERROR: could not find output from "
                          << render_product_path << std::endl;
                ovrtx_destroy_results(renderer, current_step_result);
                cuda_cleanup();
                glfwDestroyWindow(window);
                glfwTerminate();
                ovrtx_destroy_renderer(renderer);
                return 2;
            }

            result = ovrtx_map_render_var_output(renderer,
                                               color_output_handle,
                                               &map_desc,
                                               ovrtx_timeout_infinite,
                                               &rendered_output);
            if (check_and_print_error(result, "map_render_var_output")) {
                ovrtx_destroy_results(renderer, current_step_result);
                cuda_cleanup();
                glfwDestroyWindow(window);
                glfwTerminate();
                ovrtx_destroy_renderer(renderer);
                return 1;
            }

            current_map_handle = rendered_output.map_handle;
            has_mapped_output = true;

            if (rendered_output.num_tensors != 1) {
                std::fprintf(stderr,
                             "Expected 1 tensor for the color output, got %zu\n",
                             rendered_output.num_tensors);
                ovrtx_unmap_render_var_output(
                    renderer, current_map_handle, ovrtx_cuda_sync_t{});
                ovrtx_destroy_results(renderer, current_step_result);
                cuda_cleanup();
                glfwDestroyWindow(window);
                glfwTerminate();
                ovrtx_destroy_renderer(renderer);
                return 1;
            }

            DLTensor const& out_dl = *rendered_output.tensors[0].dl;
            CUarray cuda_array = reinterpret_cast<CUarray>(out_dl.data);
            // 0.3 hoisted cuda_sync from the per-buffer struct to the output,
            // where a single sync now covers every tensor.
            CUevent wait_event = reinterpret_cast<CUevent>(
                rendered_output.cuda_sync.wait_event);
            int out_width = static_cast<int>(out_dl.shape[1]);
            int out_height = static_cast<int>(out_dl.shape[0]);

            // ovrtx may signal a CUDA event when its output is ready; wait on
            // that event in our stream before issuing the interop copy.
            if (wait_event) {
                cuda_wait_event(wait_event, cuda_stream);
            }
            cuda_copy_array_to_surface(
                0, cuda_array, out_width, out_height, cuda_format, cuda_stream);
            cuEventRecord(cuda_copy_done_event, cuda_stream);
            cuStreamSynchronize(cuda_stream);

            // Return ownership of the mapped ovrtx output and tell ovrtx to
            // defer any reuse until CUDA copy_done_event has completed.
            ovrtx_cuda_sync_t copy_done_sync = {};
            copy_done_sync.wait_event =
                reinterpret_cast<uintptr_t>(cuda_copy_done_event);
            result = ovrtx_unmap_render_var_output(
                renderer, current_map_handle, copy_done_sync);
            if (check_and_print_error(result, "unmap_rendered_output")) {
                ovrtx_destroy_results(renderer, current_step_result);
                cuda_cleanup();
                glfwDestroyWindow(window);
                glfwTerminate();
                ovrtx_destroy_renderer(renderer);
                return 1;
            }
            result = ovrtx_destroy_results(renderer, current_step_result);
            if (check_and_print_error(result, "destroy_results")) {
                cuda_cleanup();
                glfwDestroyWindow(window);
                glfwTerminate();
                ovrtx_destroy_renderer(renderer);
                return 1;
            }
            has_mapped_output = false;

            read_idx = 0;
            write_idx = 1;
        }

        // Timing accumulators
        double accumulated_cpu_ms = 0.0;
        double accumulated_vulkan_ms = 0.0;
        double accumulated_cuda_ms = 0.0;
        int frame_count = 0;
        int cuda_frame_count = 0;
        int swaps_this_second = 0;
        bool defer_swapchain_recreate = false;
        auto last_print_time = std::chrono::steady_clock::now();
        auto last_step_time = std::chrono::steady_clock::now();

        // Frame time storage for final statistics (only when num_frames > 0)
        std::vector<double> all_cpu_times_ms;
        std::vector<double> all_vulkan_times_ms;
        std::vector<double> all_cuda_times_ms;

        if (num_frames > 0) {
            all_cpu_times_ms.reserve(num_frames);
            all_vulkan_times_ms.reserve(num_frames);
            all_cuda_times_ms.reserve(num_frames);
        }

        std::cerr << "Starting render loop...\n";
        if (num_frames > 0) {
            std::cerr << "Will render " << num_frames
                      << " frames then save out.png and exit\n";
        }

        int total_frames = 0;

        // =====================================================================
        // Main loop
        // =====================================================================
        // High-level flow:
        // 1) poll UI and camera updates
        // 2) observe CUDA completion and swap read/write roles
        // 3) enqueue next ovrtx->CUDA copy if CUDA is idle
        // 4) render/present last completed buffer in Vulkan
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            auto size = vk.framebuffer_size();
            if (size.x == 0 || size.y == 0) {
                glfwWaitEvents();
                continue;
            }

            auto frame_start = std::chrono::steady_clock::now();

            vk.wait_for_fence();

            if (frame_count > 0) {
                double vulkan_ms = vk.vulkan_elapsed_ms();
                accumulated_vulkan_ms += vulkan_ms;
                if (num_frames > 0) {
                    all_vulkan_times_ms.push_back(vulkan_ms);
                }
            }

            // Recreate swapchain only at safe frame boundaries to avoid
            // invalidating in-flight render/present resources.
            if (defer_swapchain_recreate) {
                vk.recreate_swapchain();
                defer_swapchain_recreate = false;
                continue;
            }

            vk.reset_fence();

            uint32_t swapchain_image_index;
            auto acquire_result = vk.acquire_next_image(swapchain_image_index);

            if (acquire_result == AcquireResult::OutOfDate) {
                vk.recreate_swapchain();
                vk.reset_fence_to_signaled();
                continue;
            } else if (acquire_result == AcquireResult::Suboptimal) {
                // Render this frame so acquire semaphore is consumed, then
                // recreate later.
                defer_swapchain_recreate = true;
            } else if (acquire_result == AcquireResult::Minimized) {
                vk.reset_fence_to_signaled();
                continue;
            }

            // Non-blocking poll: once CUDA finishes writing the next frame,
            // swap roles so Vulkan presents fresh data while CUDA starts
            // writing the other image.
            if (cuda_work_pending) {
                CUresult event_status = cuEventQuery(cuda_frame_done_event);
                if (event_status == CUDA_SUCCESS) {
                    float cuda_elapsed_ms = 0.0f;
                    cuEventElapsedTime(
                        &cuda_elapsed_ms, cuda_start_event, cuda_end_event);
                    accumulated_cuda_ms += cuda_elapsed_ms;
                    cuda_frame_count++;
                    if (num_frames > 0) {
                        all_cuda_times_ms.push_back(cuda_elapsed_ms);
                    }

                    read_timeline_value = cuda_frame_counter;
                    std::swap(read_idx, write_idx);
                    cuda_work_pending = false;
                    swaps_this_second++;
                }
            }

            // Camera edits are sent to ovrtx as a transform attribute update so
            // the next ovrtx_step renders from the new viewpoint.
            if (g_camera_dirty) {
                // [snippet:write-camera-transform]
                // Write transform to ovrtx
                ovx_string_t prim_path_str = {};
                prim_path_str.ptr = "/World/Camera";
                prim_path_str.length = strlen("/World/Camera");

                ovrtx_prim_list_t prim_list = {};
                prim_list.prim_paths = &prim_path_str;
                prim_list.num_paths = 1;

                ovrtx_attribute_type_t attr_type = {};
                attr_type.dtype.code = kDLFloat;
                attr_type.dtype.bits = 64;
                attr_type.dtype.lanes = 16;
                attr_type.is_array = false;
                attr_type.semantic = OVRTX_SEMANTIC_XFORM_MAT4x4;

                ovrtx_binding_desc_t binding = {};
                binding.prim_list = prim_list;
                binding.prims_list_handle = 0;
                binding.attribute_name.token = 0;
                char const* attr_xform = "omni:xform";
                binding.attribute_name.string.ptr = attr_xform;
                binding.attribute_name.string.length =
                    strlen(attr_xform);
                binding.attribute_type = attr_type;
                binding.prim_mode = OVRTX_BINDING_PRIM_MODE_EXISTING_ONLY;
                binding.flags = OVRTX_BINDING_FLAG_NONE;

                ovrtx_binding_desc_or_handle_t binding_desc_or_handle = {};
                binding_desc_or_handle.binding_desc = binding;
                binding_desc_or_handle.binding_handle = 0;

                glm::mat4 transform = orbit_camera.transform_matrix();
                double transform_data[16];
                float const* src = glm::value_ptr(transform);
                for (int i = 0; i < 16; ++i) {
                    transform_data[i] = static_cast<double>(src[i]);
                }

                DLTensor transform_dl = {};
                transform_dl.data = transform_data;
                transform_dl.device.device_type = kDLCPU;
                transform_dl.device.device_id = 0;
                transform_dl.ndim = 1;
                int64_t shape[1] = {1};
                transform_dl.shape = shape;
                transform_dl.strides = nullptr;
                transform_dl.byte_offset = 0;
                transform_dl.dtype.code = kDLFloat;
                transform_dl.dtype.bits = 64;
                transform_dl.dtype.lanes = 16;

                ovrtx_input_buffer_t input_buffer = {};
                input_buffer.tensors = &transform_dl;
                input_buffer.tensor_count = 1;
                input_buffer.dirty_bits = nullptr;
                input_buffer.dirty_bits_size = 0;

                enqueue_result = ovrtx_write_attribute(renderer,
                                                       &binding_desc_or_handle,
                                                       &input_buffer,
                                                       OVRTX_DATA_ACCESS_SYNC);
                if (check_and_print_error(enqueue_result, "write_attribute")) {
                    cuda_cleanup();
                    glfwDestroyWindow(window);
                    glfwTerminate();
                    ovrtx_destroy_renderer(renderer);
                    return 1;
                }
                // [/snippet:write-camera-transform]

                g_camera_dirty = false;
            }

            // Keep at most one in-flight CUDA copy chain; this bounds latency
            // and makes timeline bookkeeping deterministic.
            if (!cuda_work_pending) {
                cuEventRecord(cuda_start_event, cuda_stream);

                // Step ovrtx with real frame delta so simulation/render timing
                // matches interactive frame pacing.
                auto now = std::chrono::steady_clock::now();
                double delta_time =
                    std::chrono::duration<double>(now - last_step_time).count();
                last_step_time = now;

                // [snippet:per-frame-light-update]
                // Recompute the orbital position of each neon light and
                // push the new transforms before ovrtx_step samples the
                // scene for this frame.
                double const neon_t =
                    std::chrono::duration<double>(now - neon_anim_start).count();
                ovrtx_enqueue_result_t neon_upd_enq =
                    update_neon_lights(renderer, neon_light_paths, neon_t);
                if (check_and_print_error(neon_upd_enq,
                                          "update_neon_lights")) {
                    cuda_cleanup();
                    glfwDestroyWindow(window);
                    glfwTerminate();
                    ovrtx_destroy_renderer(renderer);
                    return 1;
                }
                // [/snippet:per-frame-light-update]

                enqueue_result = ovrtx_step(renderer,
                                            render_products,
                                            delta_time,
                                            &current_step_result);
                if (check_and_print_error(enqueue_result, "step")) {
                    cuda_cleanup();
                    glfwDestroyWindow(window);
                    glfwTerminate();
                    ovrtx_destroy_renderer(renderer);
                    return 1;
                }

                result = ovrtx_fetch_results(renderer,
                                             current_step_result,
                                             ovrtx_timeout_infinite,
                                             &outputs);
                if (check_and_print_error(result, "fetch_results")) {
                    cuda_cleanup();
                    glfwDestroyWindow(window);
                    glfwTerminate();
                    ovrtx_destroy_renderer(renderer);
                    return 1;
                }

                // Find color output (same type as detected initially)
                OutputType frame_output_type;
                color_output_handle =
                    find_color_output(outputs, frame_output_type);

                // [snippet:map-rendered-output-cuda-array]
                result = ovrtx_map_render_var_output(renderer,
                                                   color_output_handle,
                                                   &map_desc,
                                                   ovrtx_timeout_infinite,
                                                   &rendered_output);
                if (check_and_print_error(result, "map_render_var_output")) {
                    ovrtx_destroy_results(renderer, current_step_result);
                    cuda_cleanup();
                    glfwDestroyWindow(window);
                    glfwTerminate();
                    ovrtx_destroy_renderer(renderer);
                    return 1;
                }

                current_map_handle = rendered_output.map_handle;
                has_mapped_output = true;

                if (rendered_output.num_tensors != 1) {
                    std::fprintf(stderr,
                                 "Expected 1 tensor for the color output, got %zu\n",
                                 rendered_output.num_tensors);
                    ovrtx_unmap_render_var_output(
                        renderer, current_map_handle, ovrtx_cuda_sync_t{});
                    ovrtx_destroy_results(renderer, current_step_result);
                    cuda_cleanup();
                    glfwDestroyWindow(window);
                    glfwTerminate();
                    ovrtx_destroy_renderer(renderer);
                    return 1;
                }

                DLTensor const& out_dl = *rendered_output.tensors[0].dl;
                CUarray cuda_array = reinterpret_cast<CUarray>(out_dl.data);
                // 0.3 hoisted cuda_sync from the per-buffer struct to the
                // output, where a single sync covers every tensor.
                CUevent wait_event = reinterpret_cast<CUevent>(
                    rendered_output.cuda_sync.wait_event);
                int out_width = static_cast<int>(out_dl.shape[1]);
                int out_height = static_cast<int>(out_dl.shape[0]);

                // Ensure ovrtx has finished producing the mapped CUDA array
                // before launching the copy into external Vulkan memory.
                if (wait_event) {
                    cuda_wait_event(wait_event, cuda_stream);
                }

                cuda_copy_array_to_surface(write_idx,
                                           cuda_array,
                                           out_width,
                                           out_height,
                                           cuda_format,
                                           cuda_stream);
                cuEventRecord(cuda_copy_done_event, cuda_stream);

                // Hand map ownership back to ovrtx, gated by copy completion
                // event.
                ovrtx_cuda_sync_t copy_done_sync = {};
                copy_done_sync.wait_event =
                    reinterpret_cast<uintptr_t>(cuda_copy_done_event);
                result = ovrtx_unmap_render_var_output(
                    renderer, current_map_handle, copy_done_sync);
                // [/snippet:map-rendered-output-cuda-array]
                if (check_and_print_error(result, "unmap_rendered_output")) {
                    ovrtx_destroy_results(renderer, current_step_result);
                    cuda_cleanup();
                    glfwDestroyWindow(window);
                    glfwTerminate();
                    ovrtx_destroy_renderer(renderer);
                    return 1;
                }
                result = ovrtx_destroy_results(renderer, current_step_result);
                if (check_and_print_error(result, "destroy_results")) {
                    cuda_cleanup();
                    glfwDestroyWindow(window);
                    glfwTerminate();
                    ovrtx_destroy_renderer(renderer);
                    return 1;
                }
                has_mapped_output = false;

                cuEventRecord(cuda_end_event, cuda_stream);
                cuEventRecord(cuda_frame_done_event, cuda_stream);
                cuda_frame_counter++;
                // Signal timeline value N for buffer written this frame; Vulkan
                // waits on this value before sampling that image.
                cuda_signal_timeline(cuda_frame_counter, cuda_stream);
                cuda_work_pending = true;
            }

            // Render uses read buffer only; barriers below transfer queue
            // ownership between external (CUDA) and graphics queue for safe
            // sampling.
            CommandBuffer cmd = vk.command_buffer();
            cmd.begin();
            cmd.reset_query_pool(vk.timestamp_query_pool(), 0, 2);
            cmd.write_timestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                vk.timestamp_query_pool(),
                                0);

            VkImage read_image =
                vk.sampled_image(shared_images[read_idx]).image;
            cmd.image_memory_barrier(read_image,
                                     VK_IMAGE_ASPECT_COLOR_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     VK_PIPELINE_STAGE_2_NONE,
                                     VK_ACCESS_2_NONE,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                     VK_QUEUE_FAMILY_EXTERNAL,
                                     vk.queue_family());

            cmd.image_memory_barrier(
                vk.swapchain_image(swapchain_image_index),
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_NONE,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            record_rendering_state(cmd,
                                   vk,
                                   swapchain_image_index,
                                   vert_shader,
                                   frag_shader,
                                   shared_images[read_idx]);

            cmd.image_memory_barrier(
                vk.swapchain_image(swapchain_image_index),
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                VK_ACCESS_2_NONE);

            cmd.image_memory_barrier(read_image,
                                     VK_IMAGE_ASPECT_COLOR_BIT,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     VK_PIPELINE_STAGE_2_NONE,
                                     VK_ACCESS_2_NONE,
                                     vk.queue_family(),
                                     VK_QUEUE_FAMILY_EXTERNAL);

            cmd.write_timestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                vk.timestamp_query_pool(),
                                1);
            cmd.end();
            PresentResult present_result = vk.submit_and_present(
                swapchain_image_index, read_timeline_value);
            if (present_result == PresentResult::OutOfDate ||
                present_result == PresentResult::Suboptimal) {
                defer_swapchain_recreate = true;
            }

            auto frame_end = std::chrono::steady_clock::now();
            double cpu_ms = std::chrono::duration<double, std::milli>(
                                frame_end - frame_start)
                                .count();
            accumulated_cpu_ms += cpu_ms;
            if (num_frames > 0) {
                all_cpu_times_ms.push_back(cpu_ms);
            }
            frame_count++;
            total_frames++;

            if (num_frames > 0 && total_frames >= num_frames) {
                break;
            }

            auto now = std::chrono::steady_clock::now();
            double elapsed_seconds =
                std::chrono::duration<double>(now - last_print_time).count();
            if (elapsed_seconds >= 1.0) {
                double avg_cpu_ms = accumulated_cpu_ms / frame_count;
                double avg_vulkan_ms = accumulated_vulkan_ms / frame_count;
                double avg_cuda_ms =
                    (cuda_frame_count > 0)
                        ? accumulated_cuda_ms / cuda_frame_count
                        : 0.0;

                std::cerr << "Avg (ms): CPU=" << avg_cpu_ms
                          << "  Vulkan=" << avg_vulkan_ms
                          << "  CUDA=" << avg_cuda_ms
                          << "  FPS=" << (frame_count / elapsed_seconds)
                          << "\n";

                accumulated_cpu_ms = 0.0;
                accumulated_vulkan_ms = 0.0;
                accumulated_cuda_ms = 0.0;
                frame_count = 0;
                cuda_frame_count = 0;
                swaps_this_second = 0;
                last_print_time = now;
            }
        }

        cuStreamSynchronize(cuda_stream);
        vk.wait_for_fence();

        // Print frame time statistics if --num-frames was specified
        if (num_frames > 0) {
            print_frame_time_stats(total_frames,
                                   all_cpu_times_ms,
                                   all_vulkan_times_ms,
                                   all_cuda_times_ms);
        }

        // Save last rendered frame to PNG if --num-frames was specified
        if (num_frames > 0) {
            std::cerr << "Reading back frame from buffer " << read_idx
                      << "...\n";
            std::vector<uint8_t> pixels =
                cuda_read_surface_rgba8(read_idx,
                                        tex_width,
                                        tex_height,
                                        (output_type == OutputType::HdrColor)
                                            ? CudaImageFormat::Half4
                                            : CudaImageFormat::UInt8_4);

            int ok = stbi_write_png("out.png",
                                    tex_width,
                                    tex_height,
                                    4,
                                    pixels.data(),
                                    tex_width * 4);
            if (ok) {
                std::cerr << "Saved out.png (" << tex_width << "x" << tex_height
                          << ")\n";
            } else {
                std::cerr << "Failed to write out.png\n";
            }
        }

        cuEventDestroy(cuda_start_event);
        cuEventDestroy(cuda_end_event);
        cuEventDestroy(cuda_frame_done_event);
        cuEventDestroy(cuda_copy_done_event);
        cuStreamDestroy(cuda_stream);
        cuda_cleanup();
    } catch (std::exception const& e) {
        std::cerr << "Vulkan interop error: " << e.what() << "\n";
        g_orbit_camera = nullptr;
        glfwDestroyWindow(window);
        glfwTerminate();
        ovrtx_destroy_renderer(renderer);
        return 1;
    }

    g_orbit_camera = nullptr;

    glfwDestroyWindow(window);
    glfwTerminate();
    ovrtx_destroy_renderer(renderer);

    std::cerr << "Done!\n";
    return 0;
}

// Get the directory containing the running executable (cross-platform).
// Used to locate shaders and other resources relative to the binary.
static auto get_executable_dir() -> std::filesystem::path {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    return std::filesystem::path(buf).parent_path();
#elif defined(__linux__)
    return std::filesystem::canonical("/proc/self/exe").parent_path();
#else
#error "Unsupported platform"
#endif
}

static void
mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    (void)window;
    (void)mods;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        g_mouse_pressed = (action == GLFW_PRESS);
    }
}

static void
cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    (void)window;

    if (g_mouse_pressed && g_orbit_camera) {
        float delta_x = static_cast<float>(xpos - g_last_mouse_x);
        float delta_y = static_cast<float>(ypos - g_last_mouse_y);

        g_orbit_camera->update(delta_x, delta_y);
        g_camera_dirty = true;
    }

    g_last_mouse_x = xpos;
    g_last_mouse_y = ypos;
}

static void
scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)window;
    (void)xoffset;

    if (g_orbit_camera) {
        // Dolly: scroll up = zoom in (decrease distance), scroll down = zoom
        // out
        float const dolly_factor = 0.1f;
        float current_distance = g_orbit_camera->distance();
        float new_distance =
            current_distance *
            (1.0f - static_cast<float>(yoffset) * dolly_factor);

        // Clamp to reasonable range (don't let it go negative or too close)
        float const min_distance = 0.1f;
        new_distance = std::max(new_distance, min_distance);

        g_orbit_camera->set_distance(new_distance);
        g_camera_dirty = true;
    }
}

static void print_usage(char const* program_name) {
    std::cerr << "Usage: " << program_name << " [options]\n";
    std::cerr << "\nOptions:\n";
    std::cerr << "  --usd, -u <path>          USD file path or URL (default: "
                 "robot-ovrtx sample)\n";
    std::cerr
        << "  --render-product, -r <path>  Render product prim path (default: "
        << DEFAULT_RENDER_PRODUCT_PATH << ")\n";
    std::cerr << "  --up-axis, -a <Y|Z>       Scene up axis (default: Z)\n";
    std::cerr
        << "  --units <meters|centimeters>  Scene units (default: meters)\n";
    std::cerr
        << "  --num-frames, -n <N>      Render N frames then save out.png "
           "and exit\n";
    std::cerr << "  --help, -h                Show this help message\n";
}

static bool parse_args(int argc,
                       char* argv[],
                       std::string& usd_file,
                       std::string& render_product,
                       UpAxis& up_axis,
                       Units& units,
                       int& num_frames) {
    // Set defaults
    usd_file = DEFAULT_USD_FILE_URL;
    render_product = DEFAULT_RENDER_PRODUCT_PATH;
    up_axis = DEFAULT_UP_AXIS;
    units = DEFAULT_SCENE_UNITS;
    num_frames = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        } else if (arg == "--usd" || arg == "-u") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires an argument\n";
                return false;
            }
            usd_file = argv[++i];
        } else if (arg == "--render-product" || arg == "-r") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires an argument\n";
                return false;
            }
            render_product = argv[++i];
        } else if (arg == "--up-axis" || arg == "-a") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires an argument\n";
                return false;
            }
            std::string val = argv[++i];
            if (val == "Y" || val == "y") {
                up_axis = UpAxis::Y;
            } else if (val == "Z" || val == "z") {
                up_axis = UpAxis::Z;
            } else {
                std::cerr << "Error: invalid up-axis '" << val
                          << "', must be Y or Z\n";
                return false;
            }
        } else if (arg == "--units") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires an argument\n";
                return false;
            }
            std::string val = argv[++i];
            if (val == "meters" || val == "m") {
                units = Units::Meters;
            } else if (val == "centimeters" || val == "cm") {
                units = Units::Centimeters;
            } else {
                std::cerr << "Error: invalid units '" << val
                          << "', must be 'meters' or 'centimeters'\n";
                return false;
            }
        } else if (arg == "--num-frames" || arg == "-n") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires an argument\n";
                return false;
            }
            num_frames = std::stoi(argv[++i]);
            if (num_frames <= 0) {
                std::cerr << "Error: --num-frames must be a positive integer\n";
                return false;
            }
        } else {
            std::cerr << "Error: unknown option '" << arg << "'\n";
            print_usage(argv[0]);
            return false;
        }
    }

    return true;
}

template <typename ResultT>
static bool check_and_print_error(ResultT const& result,
                                  std::string_view operation) {
    if (result.status != OVRTX_API_SUCCESS) {
        ovx_string_t error = ovrtx_get_last_error();
        if (error.ptr && error.length > 0) {
            std::cerr << "ovrtx " << operation << " failed: "
                      << std::string_view(error.ptr, error.length) << "\n";
        } else {
            std::cerr << "ovrtx " << operation << " failed\n";
        }
        return true;
    }
    return false;
}

static auto vulkan_format_for_output(OutputType type) -> VkFormat {
    return (type == OutputType::HdrColor) ? VK_FORMAT_R16G16B16A16_SFLOAT
                                          : VK_FORMAT_R8G8B8A8_SRGB;
}

static auto cuda_format_for_output(OutputType type) -> CudaImageFormat {
    return (type == OutputType::HdrColor) ? CudaImageFormat::Half4
                                          : CudaImageFormat::UInt8_4;
}

static auto output_type_name(OutputType type) -> char const* {
    return (type == OutputType::HdrColor) ? "HdrColor" : "LdrColor";
}

// Keep perf reporting separate from the render loop so timing collection logic
// stays focused on sync/render decisions.
static void print_frame_time_stats(int total_frames,
                                   std::vector<double> const& cpu_times_ms,
                                   std::vector<double> const& vulkan_times_ms,
                                   std::vector<double> const& cuda_times_ms) {
    auto compute_stats = [](std::vector<double> const& times)
        -> std::tuple<double, double, double> {
        if (times.empty()) {
            return {0.0, 0.0, 0.0};
        }
        double min_val = *std::min_element(times.begin(), times.end());
        double max_val = *std::max_element(times.begin(), times.end());
        double sum = 0.0;
        for (double t : times) {
            sum += t;
        }
        double mean = sum / static_cast<double>(times.size());
        return {min_val, max_val, mean};
    };

    std::cerr << "\n=== Frame Time Statistics (" << total_frames
              << " frames) ===\n";

    auto [cpu_min, cpu_max, cpu_mean] = compute_stats(cpu_times_ms);
    std::cerr << "CPU:    min=" << cpu_min << " ms  max=" << cpu_max
              << " ms  mean=" << cpu_mean << " ms  (n=" << cpu_times_ms.size()
              << ")\n";

    auto [vk_min, vk_max, vk_mean] = compute_stats(vulkan_times_ms);
    std::cerr << "Vulkan: min=" << vk_min << " ms  max=" << vk_max
              << " ms  mean=" << vk_mean << " ms  (n=" << vulkan_times_ms.size()
              << ")\n";

    auto [cuda_min, cuda_max, cuda_mean] = compute_stats(cuda_times_ms);
    std::cerr << "CUDA:   min=" << cuda_min << " ms  max=" << cuda_max
              << " ms  mean=" << cuda_mean << " ms  (n=" << cuda_times_ms.size()
              << ")\n";
}

// Record only the draw-time graphics state for presenting the already-populated
// shared image. Ownership/layout barriers are handled by the caller.
static void record_rendering_state(CommandBuffer& cmd,
                                   VulkanContext& vk,
                                   uint32_t swapchain_image_index,
                                   ShaderHandle vert_shader,
                                   ShaderHandle frag_shader,
                                   SampledImageHandle read_image_handle) {
    VkRenderingAttachmentInfo color_attachment = {};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = vk.swapchain_image_view(swapchain_image_index);
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue.color = {
        {0.1f, 0.1f, 0.1f, 1.0f}
    };

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.offset = {0, 0};
    rendering_info.renderArea.extent = vk.swapchain_extent();
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &color_attachment;

    cmd.begin_rendering(rendering_info);

    VkExtent2D extent = vk.swapchain_extent();
    cmd.set_viewport(0.0f,
                     0.0f,
                     static_cast<float>(extent.width),
                     static_cast<float>(extent.height));
    cmd.set_scissor(0, 0, extent.width, extent.height);
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
    cmd.set_color_write_mask(
        0,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    cmd.set_vertex_input_empty();

    vk.bind_shaders(vert_shader, frag_shader);
    VkDescriptorSet desc_set = vk.descriptor_set();
    cmd.bind_descriptor_sets(
        VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout(), 0, 1, &desc_set);

    uint32_t tex_idx = vk.sampled_image(read_image_handle).descriptor_index;
    cmd.push_constants(vk.pipeline_layout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT,
                       0,
                       sizeof(tex_idx),
                       &tex_idx);

    cmd.draw(3);
    cmd.end_rendering();
}

// Search for color output in ovrtx results, preferring HdrColor over LdrColor
// Returns the output handle and sets the output_type
static auto find_color_output(ovrtx_render_product_set_outputs_t const& outputs,
                              OutputType& output_type)
    -> ovrtx_render_var_output_handle_t {
    ovrtx_render_var_output_handle_t hdr_handle = 0;
    ovrtx_render_var_output_handle_t ldr_handle = 0;

    for (size_t i = 0; i < outputs.output_count; ++i) {
        ovrtx_render_product_output_t const& product_output =
            outputs.outputs[i];
        for (size_t f = 0; f < product_output.output_frame_count; ++f) {
            ovrtx_render_product_frame_output_t const& frame =
                product_output.output_frames[f];
            for (size_t v = 0; v < frame.render_var_count; ++v) {
                ovrtx_render_product_render_var_output_t const& var =
                    frame.output_render_vars[v];
                if (!var.render_var_name.ptr) {
                    continue;
                }

                if (strncmp(var.render_var_name.ptr,
                            "HdrColor",
                            var.render_var_name.length) == 0) {
                    hdr_handle = var.output_handle;
                } else if (strncmp(var.render_var_name.ptr,
                                   "LdrColor",
                                   var.render_var_name.length) == 0) {
                    ldr_handle = var.output_handle;
                }
            }
        }
    }

    // Prefer HdrColor, fall back to LdrColor
    if (hdr_handle != 0) {
        output_type = OutputType::HdrColor;
        return hdr_handle;
    } else if (ldr_handle != 0) {
        output_type = OutputType::LdrColor;
        return ldr_handle;
    }

    return 0; // Not found
}
