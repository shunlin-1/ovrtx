// cef-thread-spike: validate that CEF MTML mode does not perturb a separate
// render thread's frame timing, and provide an interactive browser harness
// for testing CEF coexistence with other GPU-heavy apps (e.g. neon-robot-c).
//
// Run modes:
//   baseline    -- no CEF; pure render thread, establishes target distribution.
//   cef-blank   -- CEF + about:blank; verifies idle CEF MTML overhead.
//   cef-stress  -- CEF + a heavy JS/canvas stress page; worst-case scenario.
//   interactive -- opens a real, browseable Chrome-style window. Blocks until
//                  the user closes it. Use this to manually test CEF running
//                  alongside neon-robot-c (or any other process).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <windows.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"

#include "cef_app.hpp"
#include "cef_client.hpp"
#include "render_thread.hpp"

namespace {

struct Args {
    std::string mode = "baseline";   // baseline | cef-blank | cef-stress | interactive
    int duration_seconds = 10;
    std::string csv_path;            // optional, only used for timing modes
    std::string url;                 // interactive: empty -> default per mode
    bool chrome_runtime = true;      // interactive uses Chrome runtime by default
    bool overlay = false;            // interactive only: borderless + topmost + transparent
    HWND follow_hwnd = nullptr;      // overlay mode: HWND to track for position/size/visibility
};

void print_help() {
    std::printf(
        "Usage: cef-thread-spike --mode <baseline|cef-blank|cef-stress|interactive>\n"
        "                        [--duration <seconds>]   # timing modes only\n"
        "                        [--csv <path>]           # timing modes only\n"
        "                        [--url <url>]            # interactive only\n"
        "                        [--no-chrome-runtime]    # interactive only\n");
}

bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--mode") {
            const char* v = next("--mode");
            if (!v) return false;
            out.mode = v;
        } else if (a == "--duration") {
            const char* v = next("--duration");
            if (!v) return false;
            out.duration_seconds = std::atoi(v);
        } else if (a == "--csv") {
            const char* v = next("--csv");
            if (!v) return false;
            out.csv_path = v;
        } else if (a == "--url") {
            const char* v = next("--url");
            if (!v) return false;
            out.url = v;
        } else if (a == "--no-chrome-runtime") {
            out.chrome_runtime = false;
        } else if (a == "--overlay") {
            out.overlay = true;
        } else if (a == "--follow-hwnd") {
            const char* v = next("--follow-hwnd");
            if (!v) return false;
            // Accept hex (0xABCD or ABCD) or decimal.
            unsigned long long hwnd_val = std::strtoull(v, nullptr, 0);
            out.follow_hwnd = reinterpret_cast<HWND>(hwnd_val);
        } else if (a == "--help" || a == "-h") {
            print_help();
            std::exit(0);
        }
    }
    return true;
}

// Polls a target HWND's geometry / visibility and mirrors them onto the
// spike's overlay HWND. Runs on its own thread; exits cleanly when the
// target window is destroyed.
//
// Cadence: ~60 Hz (16 ms). Cheap polling (GetWindowRect + IsIconic) — only
// pushes SetWindowPos when something actually changed, so DWM cost is ~zero
// when the host is idle.
void overlay_tracker_thread(SpikeClient* client, HWND follow_hwnd,
                            std::atomic<bool>* stop) {
    if (!client || !follow_hwnd) return;

    // Wait for the spike browser to be created so we have an HWND to drive.
    HWND overlay_hwnd = nullptr;
    while (!stop->load(std::memory_order_acquire)) {
        overlay_hwnd = client->latest_hwnd();
        if (overlay_hwnd) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!overlay_hwnd) return;

    // Snap to host position immediately so the user never sees the overlay
    // pop up at Chromium's default position before drifting into place.
    {
        RECT r{};
        if (IsWindow(follow_hwnd) && GetWindowRect(follow_hwnd, &r)) {
            SetWindowPos(overlay_hwnd, HWND_TOPMOST,
                         r.left, r.top,
                         r.right - r.left, r.bottom - r.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    RECT last_rect = {-1, -1, -1, -1};
    bool last_visible = true;

    while (!stop->load(std::memory_order_acquire)) {
        // Target dead -> close the overlay (which will exit the message loop
        // via OnBeforeClose -> CefQuitMessageLoop).
        if (!IsWindow(follow_hwnd)) {
            PostMessageW(overlay_hwnd, WM_CLOSE, 0, 0);
            return;
        }

        bool minimised = IsIconic(follow_hwnd) != FALSE;
        bool target_visible = !minimised && IsWindowVisible(follow_hwnd) != FALSE;

        // Mirror visibility: hide overlay when host is minimised or hidden.
        if (target_visible != last_visible) {
            ShowWindow(overlay_hwnd, target_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            last_visible = target_visible;
        }

        if (target_visible) {
            RECT r{};
            if (GetWindowRect(follow_hwnd, &r)) {
                if (r.left   != last_rect.left   ||
                    r.top    != last_rect.top    ||
                    r.right  != last_rect.right  ||
                    r.bottom != last_rect.bottom) {
                    SetWindowPos(overlay_hwnd, HWND_TOPMOST,
                                 r.left, r.top,
                                 r.right - r.left, r.bottom - r.top,
                                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    last_rect = r;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

std::string resolve_test_page_url() {
    std::error_code ec;
    auto exe_dir = std::filesystem::current_path(ec);  // built-product cwd
    auto page = exe_dir / "test_pages" / "stress.html";
    return std::string("file:///") + page.generic_string();
}

}  // namespace

int main(int argc, char** argv) {
    // CRITICAL: this is the FIRST thing main() does. CEF re-uses the same .exe
    // for sub-processes (renderer, GPU, ...) and we must let CEF intercept
    // those sub-process invocations before our own logic runs.
    CefMainArgs cef_main_args(GetModuleHandleW(nullptr));
    {
        CefRefPtr<SpikeApp> bootstrap_app(new SpikeApp());
        int exit_code = CefExecuteProcess(cef_main_args, bootstrap_app, nullptr);
        if (exit_code >= 0) {
            // Sub-process completed; just exit with its code.
            return exit_code;
        }
    }

    Args args;
    if (!parse_args(argc, argv, args)) {
        print_help();
        return 1;
    }

    std::printf("[main] thread %lu (main thread)\n",
                static_cast<unsigned long>(GetCurrentThreadId()));
    std::printf("[main] mode = %s, duration = %d s\n",
                args.mode.c_str(), args.duration_seconds);
    std::fflush(stdout);

    bool use_cef_osr = (args.mode == "cef-blank" || args.mode == "cef-stress");
    bool use_cef_interactive = (args.mode == "interactive");
    bool use_cef = use_cef_osr || use_cef_interactive;

    CefRefPtr<SpikeApp> app;
    CefRefPtr<SpikeClient> client;
    if (use_cef) {
        CefSettings settings;
        settings.no_sandbox = true;
        settings.multi_threaded_message_loop = use_cef_osr ? 1 : 0;
        settings.windowless_rendering_enabled = use_cef_osr;
        settings.log_severity = LOGSEVERITY_WARNING;

        // Isolated user-data dir so Chromium's process singleton doesn't
        // think we're a 2nd instance of system Chrome / a previous spike
        // run. "Opening in existing browser session" + CefInitialize failure
        // is the symptom when this isn't set.
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::filesystem::path user_dir =
            std::filesystem::path(exe_path).parent_path() / "cef_user_data";
        std::filesystem::create_directories(user_dir);
        std::string user_dir_utf8 = user_dir.string();
        CefString(&settings.root_cache_path).FromString(user_dir_utf8);
        // Note: CEF 147 removed the chrome_runtime flag — Chrome runtime is
        // always the runtime now (Alloy was deprecated/removed). Interactive
        // mode therefore gets a full Chromium UI (URL bar, tabs, etc.) just by
        // not enabling windowless mode.
        (void)args.chrome_runtime;

        app = new SpikeApp();
        if (!CefInitialize(cef_main_args, settings, app, nullptr)) {
            std::fprintf(stderr, "[main] CefInitialize failed\n");
            return 2;
        }
        std::printf("[main] CefInitialize OK (MTML: CEF runs on its own thread)\n");

        client = new SpikeClient();
        if (use_cef_interactive && args.overlay) {
            client->set_overlay_mode(true);
        }

        CefWindowInfo window_info;
        if (use_cef_osr) {
            window_info.SetAsWindowless(nullptr);
        } else {
            // Default windowed mode: CEF creates its own top-level Win32 window.
            window_info.SetAsPopup(nullptr, "cef-thread-spike: interactive");
        }

        CefBrowserSettings browser_settings;
        if (use_cef_osr) {
            browser_settings.windowless_frame_rate = 60;
        }
        if (use_cef_interactive && args.overlay) {
            // Transparent background so CSS-painted areas show through and
            // the rest of the layered window stays alpha=0 (click-through).
            browser_settings.background_color = 0x00000000;
        }

        std::string url;
        if (args.mode == "cef-stress") {
            url = !args.url.empty() ? args.url : resolve_test_page_url();
        } else if (args.mode == "cef-blank") {
            url = "about:blank";
        } else {
            // interactive: default to neon.html overlay if --url not given.
            if (!args.url.empty()) {
                url = args.url;
            } else if (args.overlay) {
                std::error_code ec;
                auto exe_dir = std::filesystem::current_path(ec);
                auto page = exe_dir / "test_pages" / "neon.html";
                url = std::string("file:///") + page.generic_string();
            } else {
                url = "https://www.google.com";
            }
        }
        std::printf("[main] loading url: %s\n", url.c_str());
        std::fflush(stdout);

        CefBrowserHost::CreateBrowser(window_info, client, url,
                                      browser_settings, nullptr, nullptr);
    }

    if (use_cef_osr) {
        // Wait for the page to finish before we start measuring. The first
        // ~500ms includes V8/DOM/style startup we don't want to charge to
        // steady-state.
        auto wait_start = std::chrono::steady_clock::now();
        while (!client->load_complete() &&
               std::chrono::steady_clock::now() - wait_start <
                   std::chrono::seconds(10)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!client->load_complete()) {
            std::printf("[main] WARNING: page did not finish loading in 10s, "
                        "starting measurement anyway\n");
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (use_cef_interactive) {
        std::printf("[main] interactive mode: close the browser window to exit.\n");
        std::fflush(stdout);

        // Optional: spawn a tracker thread that mirrors the host window's
        // geometry / visibility onto our overlay HWND. Exits the overlay
        // automatically when the host window is destroyed.
        std::atomic<bool> tracker_stop{false};
        std::thread tracker;
        if (args.overlay && args.follow_hwnd) {
            std::printf("[main] tracking host HWND %p\n",
                        reinterpret_cast<void*>(args.follow_hwnd));
            std::fflush(stdout);
            tracker = std::thread(overlay_tracker_thread, client.get(),
                                  args.follow_hwnd, &tracker_stop);
        }

        // Single-threaded message loop: pump CEF on the main thread. Returns
        // when the last browser closes (we call CefQuitMessageLoop() inside
        // SpikeClient::OnBeforeClose).
        CefRunMessageLoop();
        std::printf("[main] message loop exited; shutting down CEF.\n");

        if (tracker.joinable()) {
            tracker_stop.store(true, std::memory_order_release);
            tracker.join();
        }
    } else {
        RenderThread::Config cfg;
        cfg.duration_seconds = args.duration_seconds;
        RenderThread render(cfg);
        render.start();
        render.join();

        std::printf("[main] render thread id was: %lu\n", render.thread_id());
        if (use_cef) {
            std::printf("[main] CEF OnPaint thread id was: %lu (paints=%d)\n",
                        client->paint_thread_id(), client->paint_count());
        }

        render.print_summary(args.mode);
        if (!args.csv_path.empty()) {
            render.dump_csv(args.csv_path);
        }
    }

    if (use_cef) {
        client = nullptr;
        CefShutdown();
    }
    return 0;
}
