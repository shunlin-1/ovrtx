// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Ultralight headless render PoC — first sanity check of the Ultralight
// SDK pipeline. Loads `index.html` next to the exe, renders the page to
// a CPU bitmap, writes it out as `out.png`, and exits.
//
// If this binary produces a recognisable screenshot of our test HTML,
// Ultralight's core rendering path is wired correctly and we can
// confidently move on to Vulkan integration (custom GPUDriver later).

#include <Ultralight/Ultralight.h>
#include <AppCore/Platform.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace ultralight;

int main() {
    constexpr int kWidth  = 1280;
    constexpr int kHeight = 720;

    // ─── Platform setup ──────────────────────────────────────────────
    // AppCore convenience providers handle file IO, fonts, logging, and
    // surface/clipboard implementations on the current OS — exactly what
    // we want for a headless one-shot test.
    Config config;
    Platform::instance().set_config(config);

    Platform::instance().set_font_loader(GetPlatformFontLoader());

    // File loader resolves `file://...` URLs relative to the current
    // working directory. The CMake POST_BUILD step put `index.html`
    // next to the exe, so we set CWD-style root to that.
    auto exe_dir = std::filesystem::current_path();
    Platform::instance().set_file_system(
        GetPlatformFileSystem(exe_dir.string().c_str()));

    Platform::instance().set_logger(GetDefaultLogger("ultralight-test.log"));

    // ─── Renderer + View ─────────────────────────────────────────────
    auto renderer = Renderer::Create();

    ViewConfig vc;
    vc.is_accelerated = false;   // CPU rasterizer for this PoC
    auto view = renderer->CreateView(kWidth, kHeight, vc, nullptr);

    // Load the bundled HTML.
    String url = "file:///index.html";
    view->LoadURL(url);

    std::printf("[ul] Renderer + View created, loading %s\n", url.utf8().data());
    std::fflush(stdout);

    // ─── Pump events until the page finishes loading ─────────────────
    // Ultralight is asynchronous; we poll Update + Render until
    // `is_loading()` flips to false. Cap at a few seconds so a hung
    // page doesn't spin forever.
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(10);
    while (view->is_loading()) {
        renderer->Update();
        renderer->Render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr,
                "[ul] timed out waiting for page to finish loading\n");
            break;
        }
    }
    std::printf("[ul] Page loaded; performing one more Update + Render\n");
    std::fflush(stdout);
    renderer->Update();
    renderer->Render();

    // ─── Save the rendered bitmap to PNG ─────────────────────────────
    auto* surface = view->surface();
    if (!surface) {
        std::fprintf(stderr, "[ul] view->surface() returned null\n");
        return 2;
    }
    auto* bs = static_cast<BitmapSurface*>(surface);
    auto bitmap = bs->bitmap();
    const char* out_path = "out.png";
    if (!bitmap->WritePNG(out_path)) {
        std::fprintf(stderr, "[ul] bitmap->WritePNG failed\n");
        return 3;
    }
    std::printf("[ul] DONE -> %s (%dx%d)\n", out_path,
                int(bitmap->width()), int(bitmap->height()));
    return 0;
}
