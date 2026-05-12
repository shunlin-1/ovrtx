# agv (C++) — AGV viewer in Qt6 + ovrtx C API

C++ port of `examples/python/agv`. Same architecture, no Python.
Loads an AGV USD scene through a generated sidecar (camera + render
product), runs ovrtx on a worker thread, and displays frames in a
Qt6/QML window with right-drag orbit + wheel zoom.

## Why this exists

The PySide6 version is great for prototyping but the rendering loop
does two unnecessary CPU↔GPU copies per frame and the picking helper
must run in a subprocess because ovrtx can't coexist with `pxr` in one
Python process. C++ removes both pain points and ships as a single
binary. See `examples/python/neon-robot-bim/README.md` for the longer
Qt vs Slint vs RmlUI decision log that landed us on Qt either way.

## Build prerequisites

- CMake ≥ 3.18, a C++17 compiler
- Qt 6.5+ with QML (the QML overlay uses `QtQuick.Effects.MultiEffect`)
  - Arch: `sudo pacman -S qt6-base qt6-declarative` ships 6.7+ — just install.
  - Ubuntu 24.04 / Debian 12: system Qt is **6.4**, too old.
    Install Qt 6.8 via `aqtinstall` (no sudo). Ubuntu 24.04 has PEP 668
    enabled so `pip install` is blocked — bootstrap via `pipx`:
    ```bash
    sudo apt install pipx
    pipx install aqtinstall
    ~/.local/bin/aqt install-qt linux desktop 6.8.1 linux_gcc_64 \
        -m qtshadertools -O ~/Qt
    ```
    aqt downloads under the arch *key* `linux_gcc_64` but unpacks into
    `~/Qt/6.8.1/gcc_64/` (legacy layout name). Configure CMake with
    `-DCMAKE_PREFIX_PATH=~/Qt/6.8.1/gcc_64`.
- A working ovrtx build target (auto-fetched by CMake on first
  configure, ~hundreds of MB — same as the other `examples/c/*` projects)

## Build & run

```bash
cd examples/c/agv
# Pass CMAKE_PREFIX_PATH if you installed Qt via aqtinstall (Ubuntu/Debian);
# omit it on distros where the system Qt is already 6.5+.
cmake -B build -S . -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.1/gcc_64
cmake --build build --config Release -j
./build/agv                                              # defaults to the AGV Test.usda
./build/agv --usd /path/to/scene.usda
./build/agv --rendermode "Real-Time Path-Tracing"
```

Controls:
- **Right-drag** — orbit
- **Mouse wheel** — zoom
- **Left-click** — reserved for picking (not implemented yet)

The sidecar layer (`agv_render.usda`) is regenerated next to the
source USD on every launch.

## Layout

```
agv/
├── CMakeLists.txt
├── README.md
├── qml/Main.qml                 — viewer chrome (HUD + mouse handlers)
└── src/
    ├── main.cpp                 — CLI + Qt bootstrap
    ├── sidecar.{h,cpp}          — writes the camera/render-product sidecar
    ├── orbit_camera.{h,cpp}     — Y/Z-up orbit camera math (no glm dep)
    ├── frame_image_provider.{h,cpp}  — RGBA -> QImage -> QML bridge
    └── agv_backend.{h,cpp}      — QObject + ovrtx worker thread
```

## What's in vs. what's deferred from the Python version

| Feature                              | Python | This C++ scaffold |
|--------------------------------------|:------:|:-----------------:|
| Sidecar USD generation               | ✅     | ✅                |
| Orbit camera + zoom                  | ✅     | ✅                |
| Worker-thread ovrtx_step             | ✅     | ✅                |
| CPU-mapped RGBA → QImage → QML       | ✅     | ✅                |
| Mouse picking (ray-AABB + triangle)  | ✅     | ❌ (see below)    |
| OmniPBR material mode toggling       | ✅     | ❌ (follow-on)    |
| GPU-direct frame display (Vulkan)    | ❌     | ❌ (follow-on)    |

**Why no picking yet** — the Python version off-loads the USD walk to a
subprocess running `pxr` (because ovrtx + `pxr` can't live in one
Python process). In C++ that conflict goes away: link USD's C++ libs
into the binary and call them in-process. The follow-on work is to
add `src/pick_collector.cpp` that walks the composed stage at startup
and builds the same `PickInfo` table.

## Python integration — where it slots in

The CMake flag `AGV_ENABLE_PYTHON=ON` adds CPython as a link dep but
the actual bridge code is intentionally still TODO. The clean spot to
embed Python is a new worker thread alongside `AgvBackend::runWorker`
that:

1. Calls `Py_InitializeFromConfig` once on thread start.
2. Loads a user-provided script (e.g. `ai_worker.py`) via
   `PyRun_SimpleFile`.
3. Exposes `agvBackend` and per-frame data to the script either by:
   - **Embedded interpreter (lightest)** — push commands via a
     `std::queue<std::string>` and use `PyRun_SimpleString`; cheap,
     no binding library, fine for "load an ONNX model + log inferences".
   - **pybind11 (richer)** — bind `AgvBackend`, `OrbitCamera`, and
     `FrameImageProvider` so the script can read/write them
     directly; needed for live-edit workflows.

The boundary is deliberately small. Today the C++ runs alone; the
day you need PyTorch/TensorRT/HF in the loop, add `python_bridge.cpp`
behind `AGV_ENABLE_PYTHON` and don't touch anything else.

```bash
# When that day comes:
cmake -B build -S . -DAGV_ENABLE_PYTHON=ON
```

## Known limitations / next steps

- **CPU readback per frame.** Same as the Python version. To eliminate
  the copy, port the Vulkan-interop pattern from
  `examples/c/vulkan-interop/src/main.cpp` and feed the texture handle
  straight into Qt's `QRhi`/`QQuickWindow::setGraphicsApi(Vulkan)`.
- **No camera transform caching.** Each dirty frame builds a fresh
  `ovrtx_binding_desc_t`. For >60 fps with rapid camera movement,
  cache a binding handle with `ovrtx_bind_attribute` once at startup.
- **No upAxis change at runtime.** Sidecar metadata is sniffed once.
  Re-launch to switch scenes.
