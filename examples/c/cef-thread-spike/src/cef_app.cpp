#include "cef_app.hpp"

#include <cstdio>

#include "include/cef_command_line.h"

void SpikeApp::OnContextInitialized() {
    std::fprintf(stdout,
                 "[cef-app] OnContextInitialized on thread %lu (CEF UI thread)\n",
                 static_cast<unsigned long>(GetCurrentThreadId()));
    std::fflush(stdout);
}

void SpikeApp::OnBeforeCommandLineProcessing(
    const CefString& process_type,
    CefRefPtr<CefCommandLine> command_line) {
    const bool is_browser = process_type.empty();

    // Run all GPU work inside the browser process — no separate GPU
    // sub-process. This is the nuclear fix for hybrid-GPU laptops where the
    // GPU sub-process repeatedly crashes during virtualised D3D context
    // creation. Steam Overlay / Epic Launcher / Battle.net all use this
    // switch for the same reason.
    command_line->AppendSwitch("in-process-gpu");

    // Force ANGLE to use D3D11 explicitly. "auto" can pick a backend that
    // fails virtualised context creation across the two GPUs.
    command_line->AppendSwitchWithValue("use-angle", "d3d11");

    // Disable Chromium's internal Vulkan compositor (unrelated to our app's
    // ovrtx Vulkan). Multi-GPU + Chromium Vulkan = grief.
    command_line->AppendSwitchWithValue("disable-features", "Vulkan");

    // Override the GPU blocklist + force GPU rasterization. Without these,
    // Chromium can decide hardware accel is "unsafe" and fall back to
    // software paths even when the GPU is healthy.
    command_line->AppendSwitch("ignore-gpu-blocklist");
    command_line->AppendSwitch("enable-gpu-rasterization");

    if (is_browser) {
        std::fprintf(stdout,
                     "[cef-app] applied switches: in-process-gpu, "
                     "angle=d3d11, no Vulkan compositor, ignore-blocklist\n");
        std::fflush(stdout);
    }
}
