#pragma once

#include <atomic>
#include <functional>
#include <mutex>

#include "include/cef_client.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"

// CefClient combines all the per-browser handler interfaces. We implement:
//   - CefRenderHandler: required for windowless (OSR) mode. We discard pixels
//     but count OnPaint invocations to verify CEF is producing frames.
//   - CefLoadHandler: prints when the test page finishes loading so we know
//     when to start measurement.
//   - CefLifeSpanHandler: tracks browser open/close so the main thread can
//     wait for the user to close the interactive window.
//
// The same client serves both the timing-test (OSR) and interactive-browser
// (windowed) modes; the windowing decision is made at CreateBrowser time.
class SpikeClient : public CefClient,
                    public CefRenderHandler,
                    public CefLoadHandler,
                    public CefLifeSpanHandler {
public:
    SpikeClient();

    // Enable Win32 overlay decorations on the next browser created:
    //   - WS_EX_LAYERED + LWA_ALPHA so DWM composites with per-pixel alpha
    //     and clicks on transparent areas pass through to whatever is below.
    //   - WS_EX_TOPMOST so the overlay stays above the host (ovrtx) window.
    //   - WS_POPUP (no caption / border) so the overlay looks integrated.
    void set_overlay_mode(bool enabled) { overlay_mode_ = enabled; }

    CefRefPtr<CefRenderHandler>   GetRenderHandler() override   { return this; }
    CefRefPtr<CefLoadHandler>     GetLoadHandler() override     { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser,
                 PaintElementType type,
                 const RectList& dirtyRects,
                 const void* buffer,
                 int width,
                 int height) override;

    // CefLoadHandler
    void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    int paint_count() const {
        return paint_count_.load(std::memory_order_relaxed);
    }
    bool load_complete() const {
        return load_complete_.load(std::memory_order_acquire);
    }
    unsigned long paint_thread_id() const {
        return paint_thread_id_.load(std::memory_order_relaxed);
    }

    // True once at least one browser is open and not yet fully torn down.
    bool any_browser_open() const {
        return live_browsers_.load(std::memory_order_acquire) > 0;
    }
    // True after the last browser has been closed (and we've seen at least
    // one). Used by interactive mode's main thread wait.
    bool all_browsers_closed() const {
        return ever_opened_.load(std::memory_order_acquire) &&
               live_browsers_.load(std::memory_order_acquire) == 0;
    }

    // Returns the latest browser's native window handle, or nullptr if no
    // browser is alive yet. Cached at OnAfterCreated time.
    HWND latest_hwnd() const {
        return latest_hwnd_.load(std::memory_order_acquire);
    }

private:
    std::atomic<int>           paint_count_{0};
    std::atomic<bool>          load_complete_{false};
    std::atomic<unsigned long> paint_thread_id_{0};
    std::atomic<int>           live_browsers_{0};
    std::atomic<bool>          ever_opened_{false};
    bool                       overlay_mode_{false};
    std::atomic<HWND>          latest_hwnd_{nullptr};

    IMPLEMENT_REFCOUNTING(SpikeClient);
    DISALLOW_COPY_AND_ASSIGN(SpikeClient);
};
