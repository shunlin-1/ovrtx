// SPDX-License-Identifier: BSD-3-Clause
//
// Three-phase CEF + ovrtx integration retest. Mode selected by flag:
//
//   default        -> Phase 1: headless OSR -> out.png, then exit
//   --stability    -> Phase 2: continuous OSR loop, paint+uptime log
//   --vulkan       -> Phase 3: GLFW window + Vulkan composite of OSR
//                              bitmap, mirrors the Ultralight integration
//                              path that proved stable in neon-robot-c
//
// CEF process model: when invoked as a renderer / GPU / utility
// subprocess, the same exe is re-launched with --type=... in argv.
// CefExecuteProcess() routes those calls and returns >=0 — we exit
// immediately. On the main process it returns -1 and we proceed.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_render_handler.h"
#include "include/wrapper/cef_helpers.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Phase 3 only — Vulkan + GLFW reused from neon-robot-c sample.
#include <volk.h>
#include <GLFW/glfw3.h>
#include "vk/vulkan_context.hpp"
#include "vk/command_buffer.hpp"
#include "glsl/spirv_loader.hpp"

namespace {

constexpr int kWidth  = 1920;
constexpr int kHeight = 1080;

// ════════════════════════════════════════════════════════════════════
// CEF client — handles OSR paint, lifespan, load events.
// Same class powers all three phases; consumers read the bitmap or
// counters as they need.
// ════════════════════════════════════════════════════════════════════
class BimClient : public CefClient,
                  public CefRenderHandler,
                  public CefLifeSpanHandler,
                  public CefLoadHandler {
public:
    std::atomic<bool> got_paint{false};
    std::atomic<bool> page_loaded{false};
    std::atomic<int>  paint_count{0};
    std::atomic<bool> dirty{false};

    // Latest BGRA8 bitmap. Locked because Phase 3 reads it from the
    // main thread (Vulkan upload) while OnPaint may fire from CEF's UI
    // thread on certain CEF builds. With multi_threaded_message_loop
    // disabled OnPaint is in fact invoked synchronously inside
    // CefDoMessageLoopWork(), but the lock is cheap insurance.
    std::mutex             bmp_mutex;
    std::vector<uint8_t>   bmp;

    CefRefPtr<CefBrowser>  browser;

    // ── CefClient ────────────────────────────────────────────────────
    CefRefPtr<CefRenderHandler>   GetRenderHandler()   override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler>     GetLoadHandler()     override { return this; }

    // ── CefRenderHandler ─────────────────────────────────────────────
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect = CefRect(0, 0, kWidth, kHeight);
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                 const RectList& /*dirty_rects*/, const void* buffer,
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

    // ── CefLoadHandler ───────────────────────────────────────────────
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                   int /*http*/) override {
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

    // ── CefLifeSpanHandler ───────────────────────────────────────────
    void OnAfterCreated(CefRefPtr<CefBrowser> b) override {
        browser = b;
        std::fprintf(stderr, "[cef] browser created\n");
    }

    IMPLEMENT_REFCOUNTING(BimClient);
};

class BimApp : public CefApp {
public:
    IMPLEMENT_REFCOUNTING(BimApp);
};

bool save_png_swizzled_bgra(const std::vector<uint8_t>& bgra,
                            int w, int h, const char* path) {
    std::vector<uint8_t> rgba(bgra.size());
    for (std::size_t i = 0; i < bgra.size(); i += 4) {
        rgba[i + 0] = bgra[i + 2];
        rgba[i + 1] = bgra[i + 1];
        rgba[i + 2] = bgra[i + 0];
        rgba[i + 3] = bgra[i + 3];
    }
    return stbi_write_png(path, w, h, 4, rgba.data(), w * 4) != 0;
}

// Forward decl so default_url() can resolve relative to the exe.
std::filesystem::path exe_dir();

std::string default_url() {
    auto html = exe_dir() / "ui-html" / "index.html";
    if (!std::filesystem::exists(html)) {
        std::fprintf(stderr,
            "ui-html/index.html not found at %s\n",
            html.string().c_str());
        std::exit(1);
    }
    return "file:///" + html.generic_string();
}

// ════════════════════════════════════════════════════════════════════
// Phase 3 — interactive Vulkan composite.
// ════════════════════════════════════════════════════════════════════
// Resolve a path relative to the executable's directory rather than CWD.
// The exe and its .spv shaders / CEF runtime DLLs / ui-html are all
// copied into the same Release/ folder by POST_BUILD, but if the user
// launches from a different cwd then `current_path()` no longer points
// at them. GetModuleFileNameW is the right anchor.
std::filesystem::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

int run_phase3_vulkan(const std::string& url) {
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(
        kWidth, kHeight, "cef-bim-test (Phase 3)", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    VulkanContextConfig vk_cfg;
    vk_cfg.window = window;
    vk_cfg.initial_sampled_image_capacity = 16;
    CUuuid zero_uuid = {};
    VulkanContext vk(vk_cfg, zero_uuid);

    auto vert_spirv = load_spirv(
        (exe_dir() / "shaders" / "fullscreen.vert.spv").string());
    auto frag_spirv = load_spirv(
        (exe_dir() / "shaders" / "fullscreen.frag.spv").string());
    auto [vs, fs] = vk.create_linked_vertex_and_fragment_shaders(
        vert_spirv, frag_spirv);

    SampledImageHandle ui_handle = vk.create_sampled_image(
        kWidth, kHeight, VK_FORMAT_B8G8R8A8_UNORM, VK_FILTER_LINEAR, false);

    // Initial transition UNDEFINED -> SHADER_READ.
    {
        VkImage img = vk.sampled_image(ui_handle).image;
        // immediate_submit isn't on the simplified context — open-code
        // the one-time transition through a fresh command buffer.
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pi = {};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = vk.queue_family();
        vkCreateCommandPool(vk.device(), &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb;
        vkAllocateCommandBuffers(vk.device(), &ai, &cb);
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        CommandBuffer cmd(cb);
        cmd.image_memory_barrier(
            img, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(vk.graphics_queue(), 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(vk.graphics_queue());
        vkDestroyCommandPool(vk.device(), pool, nullptr);
    }

    // Persistent host-coherent staging buffer (8 MB).
    constexpr VkDeviceSize UI_BYTES = VkDeviceSize(kWidth) * kHeight * 4;
    VkBuffer       staging        = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem    = VK_NULL_HANDLE;
    void*          staging_mapped = nullptr;
    {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = UI_BYTES;
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

    // ── CEF init ─────────────────────────────────────────────────────
    CefRefPtr<BimClient> client(new BimClient);
    {
        CefWindowInfo wi;
        wi.SetAsWindowless(0);
        CefBrowserSettings bs;
        bs.windowless_frame_rate = 60;
        CefBrowserHost::CreateBrowser(wi, client.get(), url, bs,
                                      nullptr, nullptr);
    }

    // ── GLFW input -> CEF forwarding ─────────────────────────────────
    // Stash the client + window size on the GLFW user pointer so the
    // C-style callbacks below can reach the browser. SendMouse*Event
    // dispatches to CEF's UI thread internally, so calling them from
    // the GLFW main-loop callbacks is safe.
    struct InputCtx {
        BimClient* client;
        double last_x = 0;
        double last_y = 0;
    };
    InputCtx ctx{ client.get() };
    glfwSetWindowUserPointer(window, &ctx);

    glfwSetCursorPosCallback(window,
        [](GLFWwindow* w, double xpos, double ypos) {
            auto* c = static_cast<InputCtx*>(glfwGetWindowUserPointer(w));
            if (!c->client->browser) return;
            c->last_x = xpos; c->last_y = ypos;
            int ww, wh; glfwGetWindowSize(w, &ww, &wh);
            CefMouseEvent ev;
            ev.x = static_cast<int>(xpos * kWidth  / std::max(1, ww));
            ev.y = static_cast<int>(ypos * kHeight / std::max(1, wh));
            ev.modifiers = 0;
            c->client->browser->GetHost()->SendMouseMoveEvent(ev, false);
        });

    glfwSetMouseButtonCallback(window,
        [](GLFWwindow* w, int btn, int action, int /*mods*/) {
            auto* c = static_cast<InputCtx*>(glfwGetWindowUserPointer(w));
            if (!c->client->browser) return;
            int ww, wh; glfwGetWindowSize(w, &ww, &wh);
            CefMouseEvent ev;
            ev.x = static_cast<int>(c->last_x * kWidth  / std::max(1, ww));
            ev.y = static_cast<int>(c->last_y * kHeight / std::max(1, wh));
            ev.modifiers = 0;
            cef_mouse_button_type_t cef_btn =
                (btn == GLFW_MOUSE_BUTTON_RIGHT)  ? MBT_RIGHT
              : (btn == GLFW_MOUSE_BUTTON_MIDDLE) ? MBT_MIDDLE
                                                  : MBT_LEFT;
            c->client->browser->GetHost()->SendMouseClickEvent(
                ev, cef_btn, action == GLFW_RELEASE, 1);
        });

    glfwSetScrollCallback(window,
        [](GLFWwindow* w, double dx, double dy) {
            auto* c = static_cast<InputCtx*>(glfwGetWindowUserPointer(w));
            if (!c->client->browser) return;
            int ww, wh; glfwGetWindowSize(w, &ww, &wh);
            CefMouseEvent ev;
            ev.x = static_cast<int>(c->last_x * kWidth  / std::max(1, ww));
            ev.y = static_cast<int>(c->last_y * kHeight / std::max(1, wh));
            ev.modifiers = 0;
            // Chromium expects pixel deltas; one wheel notch ~120 in
            // Win32 → 40 px feels right for our line-height.
            c->client->browser->GetHost()->SendMouseWheelEvent(
                ev, static_cast<int>(dx * 40),
                    static_cast<int>(dy * 40));
        });

    // ── Main loop ────────────────────────────────────────────────────
    auto start = std::chrono::steady_clock::now();
    auto next_log = start + std::chrono::seconds(10);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        // Pump CEF; OnPaint dirties the staging mirror.
        CefDoMessageLoopWork();

        auto sz = vk.framebuffer_size();
        if (sz.x == 0 || sz.y == 0) {
            glfwWaitEvents();
            continue;
        }

        vk.wait_for_fence();
        vk.reset_fence();

        uint32_t image_index = 0;
        auto acq = vk.acquire_next_image(image_index);
        if (acq == AcquireResult::OutOfDate) {
            vk.recreate_swapchain();
            vk.reset_fence_to_signaled();
            continue;
        }
        if (acq == AcquireResult::Minimized) {
            vk.reset_fence_to_signaled();
            continue;
        }

        CommandBuffer cmd = vk.command_buffer();
        cmd.begin();

        // Transition swapchain to color-attachment.
        cmd.image_memory_barrier(
            vk.swapchain_image(image_index),
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_NONE,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // Upload latest CEF bitmap if dirty.
        if (client->dirty.exchange(false)) {
            {
                std::lock_guard<std::mutex> lock(client->bmp_mutex);
                if (!client->bmp.empty()) {
                    std::memcpy(staging_mapped, client->bmp.data(),
                                std::min<std::size_t>(
                                    UI_BYTES, client->bmp.size()));
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
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = { uint32_t(kWidth), uint32_t(kHeight), 1 };
            vkCmdCopyBufferToImage(cmd.handle(), staging, ui_img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &region);
            cmd.image_memory_barrier(
                ui_img, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }

        // ── Composite (sampler index pair, scene = none, ui = our image) ─
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
        cmd.set_viewport(0.0f, 0.0f,
                         static_cast<float>(ext.width),
                         static_cast<float>(ext.height));
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

        vk.bind_shaders(vs, fs);
        VkDescriptorSet desc = vk.descriptor_set();
        cmd.bind_descriptor_sets(
            VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout(), 0, 1, &desc);

        // Push constants: feed our CEF UI texture into the shader's
        // "scene" slot and signal "no overlay" via UINT32_MAX in the ui
        // slot. The neon-robot-c shader's early-out branch then just
        // samples a single texture (our CEF bitmap) and emits it. Phase 4
        // will swap this back to scene = ovrtx, ui = CEF.
        uint32_t indices[2] = {
            vk.sampled_image(ui_handle).descriptor_index,  // scene_index
            UINT32_MAX,                                    // ui_index
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

        cmd.end();
        vk.submit_and_present(image_index);

        // Periodic stability log.
        auto now = std::chrono::steady_clock::now();
        if (now >= next_log) {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                now - start).count();
            std::fprintf(stderr,
                "[phase3] uptime=%4llds paints=%d\n",
                static_cast<long long>(secs),
                client->paint_count.load());
            next_log = now + std::chrono::seconds(10);
        }
    }

    vkDeviceWaitIdle(vk.device());
    vkUnmapMemory(vk.device(), staging_mem);
    vkFreeMemory(vk.device(), staging_mem, nullptr);
    vkDestroyBuffer(vk.device(), staging, nullptr);

    if (client->browser) {
        client->browser->GetHost()->CloseBrowser(true);
    }
    for (int i = 0; i < 100; ++i) {
        CefDoMessageLoopWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CefShutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    CefMainArgs main_args(::GetModuleHandle(nullptr));
#else
    CefMainArgs main_args(argc, argv);
#endif
    CefRefPtr<BimApp> app(new BimApp);

    int proc_rc = CefExecuteProcess(main_args, app.get(), nullptr);
    if (proc_rc >= 0) return proc_rc;

    // ── Parse flags ──────────────────────────────────────────────────
    std::string url;
    bool stability_mode = false;
    bool vulkan_mode    = false;
    int  stability_minutes = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--url" && i + 1 < argc) { url = argv[++i]; }
        else if (a == "--stability")           { stability_mode = true; }
        else if (a == "--vulkan")              { vulkan_mode = true; }
        else if (a == "--minutes" && i + 1 < argc) {
            stability_minutes = std::atoi(argv[++i]);
        }
    }
    if (url.empty()) url = default_url();
    std::fprintf(stderr, "[cef] loading %s\n", url.c_str());

    // ── CefSettings (shared across phases) ───────────────────────────
    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.multi_threaded_message_loop  = false;
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.cache_path) =
        (std::filesystem::current_path() / "cef-cache").string();

    if (vulkan_mode) {
        // Phase 3 owns the CEF lifetime — initialize, run window loop,
        // shutdown internally.
        if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
            std::fprintf(stderr, "CefInitialize failed\n");
            return 1;
        }
        try {
            return run_phase3_vulkan(url);
        } catch (std::exception const& e) {
            std::fprintf(stderr, "[phase3] FATAL: %s\n", e.what());
            CefShutdown();
            return 3;
        }
    }

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        std::fprintf(stderr, "CefInitialize failed\n");
        return 1;
    }

    // ── Phase 1 / 2 — headless OSR ───────────────────────────────────
    CefRefPtr<BimClient> client(new BimClient);
    {
        CefWindowInfo wi;
        wi.SetAsWindowless(0);
        CefBrowserSettings bs;
        bs.windowless_frame_rate = 30;
        CefBrowserHost::CreateBrowser(wi, client.get(), url, bs,
                                      nullptr, nullptr);
    }

    if (stability_mode) {
        // Phase 2: continuous OSR loop, paint+uptime log every 10 s.
        const auto start = std::chrono::steady_clock::now();
        auto next_log = start + std::chrono::seconds(10);
        const auto stop_at = stability_minutes > 0
            ? start + std::chrono::minutes(stability_minutes)
            : std::chrono::steady_clock::time_point::max();
        int last = 0;
        std::fprintf(stderr,
            "[stability] running %s (Ctrl+C to stop)\n",
            stability_minutes > 0
                ? (std::to_string(stability_minutes) + " min").c_str()
                : "indefinitely");
        while (std::chrono::steady_clock::now() < stop_at) {
            CefDoMessageLoopWork();
            auto now = std::chrono::steady_clock::now();
            if (now >= next_log) {
                int p = client->paint_count.load();
                auto secs =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        now - start).count();
                std::fprintf(stderr,
                    "[stability] uptime=%4llds paints=%d (+%d in 10s)\n",
                    static_cast<long long>(secs), p, p - last);
                last = p;
                next_log = now + std::chrono::seconds(10);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        std::fprintf(stderr, "[stability] elapsed clean shutdown\n");
    } else {
        // Phase 1: snapshot to PNG.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            CefDoMessageLoopWork();
            if (client->page_loaded && client->got_paint) {
                for (int i = 0; i < 30; ++i) {
                    CefDoMessageLoopWork();
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(16));
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        if (!client->got_paint) {
            std::fprintf(stderr, "[cef] timed out without a paint\n");
            CefShutdown();
            return 2;
        }
        std::fprintf(stderr, "[cef] paints=%d, saving out.png\n",
                     client->paint_count.load());
        std::vector<uint8_t> snapshot;
        {
            std::lock_guard<std::mutex> lock(client->bmp_mutex);
            snapshot = client->bmp;
        }
        if (!save_png_swizzled_bgra(snapshot, kWidth, kHeight, "out.png")) {
            std::fprintf(stderr, "stbi_write_png failed\n");
        } else {
            std::fprintf(stderr, "[cef] saved out.png\n");
        }
    }

    if (client->browser) {
        client->browser->GetHost()->CloseBrowser(true);
    }
    for (int i = 0; i < 100; ++i) {
        CefDoMessageLoopWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CefShutdown();
    return 0;
}
