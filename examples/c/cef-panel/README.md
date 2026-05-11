# cef-panel — CEF embedded as a child HWND

Single Win32 main window with a Chromium Embedded Framework browser
embedded as a child HWND via `SetAsChild` + Alloy runtime. This is the
original CEF embedding pattern, known to work reliably for native
desktop hosts before the off-screen-rendering (OSR) variants.

The goal is to validate a baseline integration: CEF starts, loads a
page, resizes with the parent window, and shuts down cleanly — no
fancy compositing, no off-screen buffer, no thread juggling.

## Run

```powershell
cd examples\c\cef-panel
.\configure.bat   # cmake configure (auto-downloads CEF on first run)
.\build.bat       # cmake build
.\run.bat         # launches cef-panel.exe -> test_pages/neon.html
```

Override the page:

```powershell
.\build\Release\cef-panel.exe --url https://example.com
```

## Notes

* CEF binary distribution (~250 MB) is downloaded by CMake on first
  configure and cached under `build/cef_binary_<version>_windows64/`.
* Windows-only (uses Win32 HWND + WinMain).
* `test_pages/neon.html` is a small offline page used to smoke-test
  rendering without a network.
