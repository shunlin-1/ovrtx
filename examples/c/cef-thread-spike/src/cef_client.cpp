#include "cef_client.hpp"

#include <cstdio>

#include <windows.h>

#include "include/cef_app.h"  // CefQuitMessageLoop

namespace {
constexpr int kViewWidth = 1920;
constexpr int kViewHeight = 1080;
}  // namespace

SpikeClient::SpikeClient() = default;

void SpikeClient::GetViewRect(CefRefPtr<CefBrowser> /*browser*/,
                              CefRect& rect) {
    rect = CefRect(0, 0, kViewWidth, kViewHeight);
}

void SpikeClient::OnPaint(CefRefPtr<CefBrowser> /*browser*/,
                          PaintElementType type,
                          const RectList& /*dirtyRects*/,
                          const void* /*buffer*/,
                          int /*width*/,
                          int /*height*/) {
    if (type != PET_VIEW) return;

    // Record (once) which thread CEF dispatches OnPaint on. This must NOT be
    // the main thread or the render thread.
    unsigned long tid = static_cast<unsigned long>(GetCurrentThreadId());
    unsigned long expected = 0;
    paint_thread_id_.compare_exchange_strong(expected, tid,
                                             std::memory_order_relaxed);

    paint_count_.fetch_add(1, std::memory_order_relaxed);
}

void SpikeClient::OnLoadEnd(CefRefPtr<CefBrowser> /*browser*/,
                            CefRefPtr<CefFrame> frame,
                            int httpStatusCode) {
    if (!frame->IsMain()) return;
    std::fprintf(stdout,
                 "[cef-client] page load complete (http %d)\n",
                 httpStatusCode);
    std::fflush(stdout);
    load_complete_.store(true, std::memory_order_release);
}

void SpikeClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    int n = live_browsers_.fetch_add(1, std::memory_order_acq_rel) + 1;
    ever_opened_.store(true, std::memory_order_release);
    std::fprintf(stdout, "[cef-client] browser created (live=%d)\n", n);
    std::fflush(stdout);

    HWND hwnd = browser->GetHost()->GetWindowHandle();
    latest_hwnd_.store(hwnd, std::memory_order_release);

    if (overlay_mode_) {
        if (!hwnd) {
            std::fprintf(stderr,
                         "[cef-client] overlay: GetWindowHandle returned null; "
                         "transparent overlay will not apply\n");
            return;
        }

        LONG_PTR old_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        LONG_PTR old_ex    = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

        // Replace the style entirely (not bitmask). Chromium chrome runtime's
        // default style includes WS_OVERLAPPEDWINDOW which it uses to drive
        // its non-client area; clearing it cleanly is more reliable than
        // turning off individual bits one at a time.
        LONG_PTR new_style = WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN;
        SetWindowLongPtrW(hwnd, GWL_STYLE, new_style);

        // WS_EX_LAYERED   = DWM per-pixel alpha compositing.
        // WS_EX_TOPMOST   = stay above the host (ovrtx) window.
        // WS_EX_NOACTIVATE= clicks don't steal focus from the host.
        // WS_EX_TOOLWINDOW= no taskbar entry, suppresses default chrome on
        //                   some Windows window managers + chrome runtime.
        LONG_PTR new_ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE
                        | WS_EX_TOOLWINDOW;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new_ex);

        // 255 = window-level alpha is opaque, so Chromium's per-pixel alpha
        // (which it writes to the layered surface) drives final transparency.
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

        // Force a non-client recompute. SWP_FRAMECHANGED flushes the new
        // GWL_STYLE into the window's frame.
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED
                     | SWP_NOACTIVATE | SWP_SHOWWINDOW);

        std::fprintf(stdout,
                     "[cef-client] overlay HWND=%p\n"
                     "             style:    0x%08llX -> 0x%08llX\n"
                     "             ex_style: 0x%08llX -> 0x%08llX\n",
                     reinterpret_cast<void*>(hwnd),
                     static_cast<unsigned long long>(old_style),
                     static_cast<unsigned long long>(new_style),
                     static_cast<unsigned long long>(old_ex),
                     static_cast<unsigned long long>(new_ex));
        std::fflush(stdout);
    }
}

void SpikeClient::OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) {
    int n = live_browsers_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    std::fprintf(stdout, "[cef-client] browser closed (live=%d)\n", n);
    std::fflush(stdout);
    // If we are running CEF on the main thread (single-threaded message loop),
    // we need to break out of CefRunMessageLoop once the last browser closes.
    // CefQuitMessageLoop is a no-op under MTML mode, so always-call is safe.
    if (n == 0) {
        CefQuitMessageLoop();
    }
}
