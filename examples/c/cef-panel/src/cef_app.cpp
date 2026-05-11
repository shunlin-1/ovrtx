#include "cef_app.hpp"

#include <cstdio>
#include <windows.h>

#include "include/cef_command_line.h"

void PanelApp::OnContextInitialized() {
    std::fprintf(stdout, "[cef] OnContextInitialized on thread %lu\n",
                 static_cast<unsigned long>(GetCurrentThreadId()));
    std::fflush(stdout);
}

void PanelApp::OnBeforeCommandLineProcessing(
    const CefString& process_type,
    CefRefPtr<CefCommandLine> command_line) {
    // Hybrid-GPU laptop workaround.
    command_line->AppendSwitch("in-process-gpu");
    command_line->AppendSwitch("ignore-gpu-blocklist");
    command_line->AppendSwitch("enable-gpu-rasterization");

    if (process_type.empty()) {
        std::fprintf(stdout,
                     "[cef] applied switches: in-process-gpu, "
                     "ignore-blocklist, enable-gpu-rasterization\n");
        std::fflush(stdout);
    }
}
