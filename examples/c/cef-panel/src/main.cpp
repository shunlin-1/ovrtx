// cef-panel: single Win32 main window with CEF browser embedded as a child
// HWND via SetAsChild + Alloy runtime. The original CEF embedding pattern,
// known to work reliably for native-window hosts.
//
// Usage:
//   cef-panel.exe                       loads test_pages/neon.html
//   cef-panel.exe --url <url>           loads <url>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"

#include "cef_app.hpp"
#include "cef_client.hpp"

namespace {

constexpr wchar_t kClassName[]   = L"cef-panel-main";
constexpr wchar_t kWindowTitle[] = L"cef-panel (Alloy + SetAsChild)";

CefRefPtr<PanelClient> g_client;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            if (g_client) {
                HWND child = g_client->browser_hwnd();
                if (child) {
                    RECT r;
                    GetClientRect(hwnd, &r);
                    SetWindowPos(child, nullptr, 0, 0,
                                 r.right - r.left, r.bottom - r.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            // Politely close the browser; PanelClient::OnBeforeClose will
            // PostQuitMessage when the last browser is gone.
            if (g_client) {
                if (auto b = g_client->browser()) {
                    b->GetHost()->CloseBrowser(false);
                    return 0;
                }
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::string default_url() {
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring exe_dir = exe_path;
    size_t slash = exe_dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exe_dir.resize(slash);
    std::wstring url_w = L"file:///" + exe_dir + L"/test_pages/neon.html";
    return std::string(url_w.begin(), url_w.end());
}

struct Args {
    std::string url;
    int w = 1280;
    int h = 800;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            a.url = argv[++i];
        } else if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            a.w = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            a.h = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: cef-panel [--url <url>] [--width N] [--height N]\n");
            std::exit(0);
        }
    }
    if (a.url.empty()) a.url = default_url();
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    CefMainArgs main_args(GetModuleHandleW(nullptr));

    // Sub-process gate.
    int code = CefExecuteProcess(main_args, nullptr, nullptr);
    if (code >= 0) return code;

    Args args = parse(argc, argv);

    CefSettings settings;
    settings.no_sandbox = true;
    // MTML so CEF runs on its own threads; we run a Win32 GetMessage loop.
    settings.multi_threaded_message_loop = true;
    settings.windowless_rendering_enabled = false;
    settings.log_severity = LOGSEVERITY_WARNING;

    // Isolated user-data dir.
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring exe_dir = exe_path;
    size_t slash = exe_dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exe_dir.resize(slash);
    std::wstring user_dir_w = exe_dir + L"\\cef_user_data";
    CreateDirectoryW(user_dir_w.c_str(), nullptr);
    std::string user_dir(user_dir_w.begin(), user_dir_w.end());
    CefString(&settings.root_cache_path).FromString(user_dir);

    CefRefPtr<PanelApp> app(new PanelApp());
    if (!CefInitialize(main_args, settings, app, nullptr)) {
        std::fprintf(stderr, "[main] CefInitialize failed (code %d)\n",
                     CefGetExitCode());
        return CefGetExitCode();
    }
    std::fprintf(stdout, "[main] CefInitialize OK\n"); std::fflush(stdout);

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = kClassName;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    HWND main_hwnd = CreateWindowExW(
        0, kClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, args.w, args.h,
        nullptr, nullptr, hinst, nullptr);
    if (!main_hwnd) {
        std::fprintf(stderr, "[main] CreateWindow failed (LastError=%lu)\n",
                     GetLastError());
        CefShutdown();
        return 3;
    }
    ShowWindow(main_hwnd, SW_SHOW);
    UpdateWindow(main_hwnd);
    std::fprintf(stdout, "[main] main_hwnd=%p\n",
                 reinterpret_cast<void*>(main_hwnd)); std::fflush(stdout);

    // Build a CEF browser as child of main_hwnd, using Alloy runtime for
    // reliable Win32 SetAsChild support.
    g_client = new PanelClient();

    RECT cr{};
    GetClientRect(main_hwnd, &cr);

    CefWindowInfo wi;
    wi.SetAsChild(main_hwnd, CefRect(0, 0, cr.right, cr.bottom));
    wi.runtime_style = CEF_RUNTIME_STYLE_ALLOY;

    CefBrowserSettings bs;

    std::fprintf(stdout, "[main] loading url: %s\n", args.url.c_str());
    std::fflush(stdout);
    bool created = CefBrowserHost::CreateBrowser(wi, g_client, args.url, bs,
                                                 nullptr, nullptr);
    std::fprintf(stdout, "[main] CreateBrowser returned %d\n", int(created));
    std::fflush(stdout);

    // Win32 message loop. CEF runs on its own threads (MTML).
    std::fprintf(stdout, "[main] entering message loop\n"); std::fflush(stdout);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    std::fprintf(stdout, "[main] message loop exited\n"); std::fflush(stdout);

    g_client = nullptr;
    CefShutdown();
    return 0;
}
