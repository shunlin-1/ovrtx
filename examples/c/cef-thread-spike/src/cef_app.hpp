#pragma once

#include "include/cef_app.h"

// Browser-process app. Customizes Chromium command-line switches before CEF
// hands them to the GPU/renderer sub-processes, so we can work around the
// hybrid-GPU laptop "Failed to create shared context for virtualization" crash.
class SpikeApp : public CefApp, public CefBrowserProcessHandler {
public:
    SpikeApp() = default;

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }

    // Called on the browser process UI thread once CEF has fully initialised.
    // Useful as a sync point for diagnostics.
    void OnContextInitialized() override;

    // Called BEFORE CEF processes its own command-line, in every process
    // (browser + renderer + GPU). process_type == "" means browser process.
    // We add switches that try to keep GPU process alive on multi-GPU laptops.
    void OnBeforeCommandLineProcessing(
        const CefString& process_type,
        CefRefPtr<CefCommandLine> command_line) override;

private:
    IMPLEMENT_REFCOUNTING(SpikeApp);
    DISALLOW_COPY_AND_ASSIGN(SpikeApp);
};
