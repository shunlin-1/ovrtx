#include "cef_client.hpp"

#include <cstdio>

#include "include/cef_app.h"

void PanelClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    browser_ = browser;
    HWND hwnd = browser->GetHost()->GetWindowHandle();
    browser_hwnd_.store(hwnd, std::memory_order_release);
    int n = live_browsers_.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::fprintf(stdout, "[cef] browser %d created, hwnd=%p\n",
                 n, reinterpret_cast<void*>(hwnd));
    std::fflush(stdout);
}

bool PanelClient::DoClose(CefRefPtr<CefBrowser> /*browser*/) {
    // Allow CEF default close behaviour.
    return false;
}

void PanelClient::OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) {
    int n = live_browsers_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    std::fprintf(stdout, "[cef] browser closed (live=%d)\n", n);
    std::fflush(stdout);
    if (n == 0) {
        browser_ = nullptr;
        browser_hwnd_.store(nullptr, std::memory_order_release);
        // MTML mode: post WM_QUIT so our Win32 GetMessage loop exits.
        PostQuitMessage(0);
    }
}

void PanelClient::OnLoadEnd(CefRefPtr<CefBrowser> /*browser*/,
                            CefRefPtr<CefFrame> frame, int code) {
    if (!frame->IsMain()) return;
    std::fprintf(stdout, "[cef] page load complete (http %d)\n", code);
    std::fflush(stdout);
}
