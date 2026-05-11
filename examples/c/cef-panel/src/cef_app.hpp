#pragma once
//
// PanelApp drives CEF startup.
//
// We use the **Alloy runtime + SetAsChild** path: a single Win32 main window
// hosts the CEF browser as a child HWND. Chrome runtime + SetAsChild was
// unstable in CEF 147; Alloy is the older Win32-friendly runtime that
// embeds reliably. cefsimple's `--use-native` path is the pattern.
//
// Browser is created from main() AFTER the parent HWND exists, not from
// OnContextInitialized — Alloy does not require Views.

#include <string>

#include "include/cef_app.h"

class PanelApp : public CefApp, public CefBrowserProcessHandler {
public:
    PanelApp() = default;

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }

    void OnContextInitialized() override;

    void OnBeforeCommandLineProcessing(
        const CefString& process_type,
        CefRefPtr<CefCommandLine> command_line) override;

private:
    IMPLEMENT_REFCOUNTING(PanelApp);
    DISALLOW_COPY_AND_ASSIGN(PanelApp);
};
