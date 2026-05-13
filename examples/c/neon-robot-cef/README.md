# neon-robot-cef

ovrtx + CEF + Vulkan composite, in one binary, one window.

This is `cef-bim-test` Phase 4 ("scene = ovrtx, ui = CEF") delivered as
a sibling project that mirrors the structure of `neon-robot-c`. The
scene is the same 12-light neon ring around the GR1T2 robot from
`neon-robot-c`. The UI is the same Glassmorphism / Neumorphism BIM
viewer HTML that `cef-bim-test` and the `ultralight-test` projects use
as their shared comparison page.

## Pipeline

```
 ovrtx step  ─▶ CUDA copy ─▶ Vulkan shared image (scene)  ─┐
                                                           ├─▶ fullscreen.frag ─▶ swapchain
 CEF OSR     ─▶ CPU bitmap ─▶ Vulkan sampled image (UI)  ──┘
```

The composite shader is `cef-bim-test`'s two-texture fragment — it
samples the ovrtx output sharp where the UI is transparent, and
applies a 5×5 Gaussian blur to the scene under glass UI panels so the
HTML `backdrop-filter` extends naturally across the 3D background.

## Input routing

| Input | Goes to |
|---|---|
| Left click / hover | CEF (HTML UI) |
| Right-drag | Orbit camera (ovrtx scene) |
| Scroll wheel | Camera dolly (ovrtx scene) |
| Mouse move | CEF (so hover effects work) |

Right-drag and scroll are intentionally **not** forwarded to CEF so
the UI doesn't fight the camera; everything else is forwarded.

## Build prerequisites

**Both platforms:**

- Vulkan SDK with `glslc` on PATH
- CUDA Toolkit 12+
- CMake 3.21+

**Windows:**

- Visual Studio 2022 (or 2026) with the C++ desktop workload
- ovrtx's prebuilt Windows package will be fetched into `_deps/`

**Linux:**

```bash
# Ubuntu 24.04 (Noble) — note the t64 suffix on libasound after the
# time64 ABI transition. On older Ubuntu the package is `libasound2`.
sudo apt install build-essential cmake glslang-tools \
    libgtk-3-dev libnss3 libxss1 libasound2t64 libgbm1

# Optional (faster incremental builds; configure.sh falls back to make if absent):
sudo apt install ninja-build
```

ovrtx's prebuilt `manylinux_2_35_x86_64` (or aarch64) package will be
fetched into `_deps/`.

## Build & run

### Windows

```powershell
cd examples\c\neon-robot-cef
.\configure.bat     REM cmake configure (downloads CEF on first run)
.\build.bat         REM Release build
.\run.bat           REM launches build\Release\neon-robot-cef.exe
```

### Linux

```bash
cd examples/c/neon-robot-cef
./configure.sh      # cmake configure (downloads CEF Linux tarball ~300 MB)
./build.sh          # Release build
./run.sh            # launches build/neon-robot-cef
```

## Run-time flags

| Flag | Default | Notes |
|---|---|---|
| `--usd <path>` | `neon-only.usda` (next to exe) | Override the USD scene |
| `--render-product <prim>` | `/Render/Camera` | RenderProduct prim path |
| `--url <file://... or https://...>` | `file:///<exe>/ui-html/index.html` | Override the HTML UI |

Example: load a different HTML page over the scene —

```
./run.sh --url https://example.com
```

## What's in this directory

```
neon-robot-cef/
├── CMakeLists.txt            CEF (per-platform tarball) + ovrtx + Vulkan
├── configure.{bat,sh}        cmake configure
├── build.{bat,sh}            cmake --build
├── run.{bat,sh}              launch the binary
├── neon-only.usda            scene file (copied from neon-robot-c)
├── shaders/
│   ├── fullscreen.vert       (from neon-robot-c)
│   └── fullscreen.frag       2-texture composite (from cef-bim-test)
├── ui-html/
│   └── index.html            BIM viewer HTML (from cef-bim-test)
└── src/
    ├── main.cpp              ovrtx + CEF + Vulkan glue
    ├── camera/               (copied from neon-robot-c)
    ├── cuda/                 (copied from neon-robot-c)
    ├── glsl/                 (copied from neon-robot-c)
    ├── vk/                   (copied from neon-robot-c)
    └── stb_image_write.h     PNG writer (copied)
```

## Known caveats

- **First configure downloads CEF (~250–300 MB).** Cached under
  `build/cef_binary_<version>_<platform>/`.
- **Compilation time:** CEF's `libcef_dll_wrapper` is ~50 source files
  and takes 5–10 min on the first build. Incremental builds are fast.
- **GPU process sandbox:** CEF's separate GPU process may crash when
  launched from a non-interactive shell (CI, some remote-shell setups
  on Linux). Run from a regular terminal session for a real GPU
  context.
- **CRT mixing (Windows):** ovrtx and CEF must share a CRT. This
  project forces `/MD` (dynamic CRT) for both via
  `CEF_RUNTIME_LIBRARY_FLAG`. Don't switch back to `/MT` unless you
  also rebuild ovrtx from source against `/MT`.
- **18-minute stability:** the older `cef-vulkan-bench` died with
  `DEVICE_LOST` around the 18-minute mark on CEF 138-ish. CEF 147 +
  the simpler upload-once-per-paint path used here has not been
  observed to crash, but a long uptime test is the only proof. Track
  it with the `[loop] uptime=...` log line printed every 10 s.

## Comparison with sibling projects

| Project | Window | UI engine | ovrtx? | Purpose |
|---|---|---|---|---|
| `neon-robot-c` | Vulkan | none | yes | Bare ovrtx animation, no UI |
| `cef-panel` | Win32 HWND | CEF (windowed) | no | Sanity: does CEF embed at all? |
| `cef-thread-spike` | Win32 (interactive) | CEF (windowed) | no | Threading study + manual coexistence |
| `cef-bim-test` (Ph. 3) | Vulkan | CEF (OSR) | no | CEF rasterizing into a Vulkan texture |
| **`neon-robot-cef` (this)** | **Vulkan** | **CEF (OSR)** | **yes** | **Full ovrtx + CEF composite** |
| `ultralight-test` | none (headless) | Ultralight | no | Ultralight as a CEF alternative |
