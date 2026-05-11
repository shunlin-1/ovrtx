# cef-bim-test

Fresh CEF + ovrtx integration test. Three goals, three phases:

| Phase | Question being tested | What it builds |
|-------|----------------------|----------------|
| **1** *(this)* | Does CEF render our BIM HTML at all? | One-shot OSR render to `out.png` |
| **2** | Does CEF + Vulkan window survive >20 minutes? *(the previous `cef-vulkan-bench` died with `DEVICE_LOST` around the 18-minute mark — re-test on a fresh, latest-CEF setup.)* | Live Vulkan window with continuous OSR upload |
| **3** | Full parity with `examples/c/neon-robot-c` Ultralight integration | + ovrtx scene + composite shader + input forwarding |

This project re-uses the same `ui-html/index.html` (Glassmorphism / Neumorphism BIM viewer) the Ultralight integration uses, so any rendering difference is purely the engine.

## Phase 1 build & run

Phase 1 is a CMake-only build. CEF (Chromium 147) is fetched on first
configure (~150 MB download).

```powershell
cd C:\ProjectRelated\Omniverse\cef-bim-test
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target cef-bim-test
.\build\Release\cef-bim-test.exe
# -> writes out.png (1920x1080) in the current dir
```

To render a different page (e.g. one of the `test_pages/`):

```powershell
.\build\Release\cef-bim-test.exe --url file:///C:/path/to/page.html
```

## Compare with Ultralight

The same HTML file lives at:

| Engine | Path |
|--------|------|
| Ultralight | `examples/c/neon-robot-c/ui-html/index.html` |
| CEF (this) | `cef-bim-test/ui-html/index.html` |

Ultralight's CPU rasterizer paints WebKit-flavoured CSS; CEF paints
Chromium 147. Diffing the two `out.png`s reveals how each engine
handles glass tints, gradients, neumorphic shadows, and font rendering.

## What's not tested in Phase 1

- **Stability over time** — Phase 1 exits within seconds. The "18 min
  crash" claim from `cef-vulkan-bench` requires Phase 2's continuous
  loop to reproduce or refute.
- **Vulkan compositing** — Phase 1 reads the OSR bitmap on the CPU
  and writes PNG. Phase 2 will route it into a Vulkan sampled image.
- **ovrtx scene** — added in Phase 3.

## Files

```
cef-bim-test/
├── CMakeLists.txt        Fetches CEF, builds wrapper + this exe
├── src/
│   ├── main.cpp          Phase 1 headless OSR → PNG
│   └── stb_image_write.h Single-header PNG writer
├── ui-html/              BIM viewer HTML (mirror of Ultralight integration)
├── test_pages/           Chromium-only feature demos for the comparison test
└── README.md
```
