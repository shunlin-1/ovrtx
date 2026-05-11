#pragma once
//
// Minimum CefClient for SetAsChild/Alloy embedding. Tracks browser lifetime
// and exposes the browser's child HWND so the parent window can resize it
// on WM_SIZE.

#include <atomic>

#include "include/cef_client.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"

class PanelClient : public CefClient,
                    public CefLifeSpanHandler,
                    public CefLoadHandler {
public:
    PanelClient() = default;

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler>     GetLoadHandler() override     { return this; }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;

    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override;

    HWND browser_hwnd() const {
        return browser_hwnd_.load(std::memory_order_acquire);
    }

    CefRefPtr<CefBrowser> browser() {
        return browser_;
    }

private:
    CefRefPtr<CefBrowser> browser_;
    std::atomic<HWND>     browser_hwnd_{nullptr};
    std::atomic<int>      live_browsers_{0};

    IMPLEMENT_REFCOUNTING(PanelClient);
    DISALLOW_COPY_AND_ASSIGN(PanelClient);
};
