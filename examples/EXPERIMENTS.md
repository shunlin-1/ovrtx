# Fork-specific experiments

This file is **not part of NVIDIA's upstream** `ovrtx`. It indexes the
personal R&D projects added to this fork (`github.com/shunlin-1/ovrtx`)
that live alongside the NVIDIA-shipped examples. See
[`README.md`](README.md) for the upstream examples (minimal,
vulkan-interop, planet-system, neon-robot).

Each project's own `README.md` is the source of truth for build /
run details. The table below is a one-line orientation.

## Python — ovrtx-rendered viewers

| Project | What it is | Notes |
|---|---|---|
| [`python/agv/`](python/agv/) | AGV scene viewer (PySide6 + QML) with click-to-toggle Neon / Xray / Xray-Light material modes | Y-up cm-units; sidecar USD generated at launch; pick via `pxr` subprocess. `uv run main.py`. |
| [`python/neon-robot-bim/`](python/neon-robot-bim/) | Earlier prototype — Qt/QML BIM-style overlay UI (glass / neumorph / hover / drag), `agv` is the cleaned successor | Stage-1 placeholder backdrop; AGV picked up the ovrtx-rendered backdrop in Stage 2. |
| [`python/ovrtx-stream/`](python/ovrtx-stream/) | Streams any USD scene over WebRTC (ovrtx renders, [ovstream](https://github.com/NVIDIA-Omniverse/ovstream) transports); generates a camera/RenderProduct/light sidecar when the scene has none | `--stereo` for side-by-side two-eye output, `--benchmark` for frame-cost measurement, `--png` to check a render without touching the streaming stack. `uv run main.py --serve-client`. |
| [`python/commercial-showroom/`](python/commercial-showroom/) | Renders the Commercial_NVD ArchVis furniture pack (`assets/Commercial_NVD@10013/`) by generating a sidecar stage | Z-up cm-units; the pack is a prop library with no camera/lights/RenderProduct. `@` in the pack dir name forces `@@@...@@@` asset paths. Rendering unverified (pack is 5.7 GB). `uv run main.py --list`. |

## Hybrid (C++ UI + Python helpers)

| Project | What it is | Notes |
|---|---|---|
| [`hybrid/agv/`](hybrid/agv/) | Qt6/QML + ovrtx C API viewer with Python `uv` helpers for USD work | C++ owns renderer + UI; Python subprocesses (`pick_collector_bin.py`, future AI workers) handle anything that needs `pxr`/`torch`/etc. Avoids the `usd-core` vs `ovrtx` coexistence problem by keeping Python in separate processes. Build: `cmake -B build -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.1/gcc_64 && cmake --build build`. |

## C++ — UI-embedding R&D (CEF / Ultralight)

These projects sit *alongside* ovrtx — they test web-engine embedding
strategies for overlaying HTML UI on top of a 3D viewport. Not all of
them currently call `ovrtx_*` themselves; some are isolated spikes to
de-risk a single design question.

| Project | Engine | Question being tested |
|---|---|---|
| [`c/cef-bim-test/`](c/cef-bim-test/) | CEF | Off-screen CEF compositing with Vulkan + ovrtx in one pipeline |
| [`c/cef-panel/`](c/cef-panel/) | CEF | Baseline: CEF embedded as a Win32 child HWND (SetAsChild + Alloy) |
| [`c/cef-thread-spike/`](c/cef-thread-spike/) | CEF | Can CEF in multi-threaded message-loop mode coexist with a dedicated render thread without disturbing frame timing? |
| [`c/neon-robot-cef/`](c/neon-robot-cef/) | CEF | Full integration: neon-robot-c's ovrtx pipeline + CEF OSR composited via Vulkan ("cef-bim-test Phase 4"). Cross-platform (Win + Linux). |
| [`c/openxr-stub/`](c/openxr-stub/) | OpenXR | Can an ovrtx app hold an OpenXR session? Builds and runs; reports `XR_ERROR_RUNTIME_UNAVAILABLE` without a runtime, and the CloudXR extension list with one. Fetches OpenXR-SDK at configure time. |
| [`c/ultralight-test/`](c/ultralight-test/) | Ultralight | Smallest possible Ultralight integration: HTML → CPU bitmap → PNG. Smoke-test for SDK layout and headless render path. |

## Web

| Project | What it is |
|---|---|
| [`web/ui-demo/`](web/ui-demo/) | Single-file HTML exercising glassmorphism / neumorphism / hover / drag on a `transparent` background — the test page the CEF / Ultralight integrations are designed to display on top of an ovrtx scene. |

## Streaming and XR notes

Written 2026-08-18 while spiking NVIDIA's streaming and XR libraries against
this fork. The code is in `python/ovrtx-stream/` and `c/openxr-stub/` above;
these are the findings and measurements behind it.

| Doc | What it covers |
|---|---|
| [`../notes/ovstream-xr/README.md`](../notes/ovstream-xr/README.md) | Overview: ovrtx + [ovstream](https://github.com/NVIDIA-Omniverse/ovstream) + CloudXR, and which answers two open items in `Omniverse_Kit_WebUI_架構決策.md` |
| [`../notes/ovstream-xr/cloudxr-and-openxr.md`](../notes/ovstream-xr/cloudxr-and-openxr.md) | Why the streamer *is* the OpenXR runtime; CloudXR container wiring; the XR frame budget, measured |

## How they relate

```
                    +-------------------+
                    |  ovrtx (renderer) |
                    +---------+---------+
                              |
        +---------------------+--------------------+
        |                                          |
  Python clients                              C++ clients
        |                                          |
  +-----+----+                            +--------+--------+
  | agv      |                            | cef-bim-test    |  <-- compose
  | (PySide6)|                            | (Vulkan + CEF)  |      ovrtx + UI
  +----------+                            +-----------------+
  | neon-rbt-|                            | cef-panel       |  <-- baseline:
  | bim (WIP)|                            | (HWND embedding)|      CEF window
  +----------+                            +-----------------+
                                          | cef-thread-spike|  <-- threading
                                          +-----------------+      study
                                          | ultralight-test |  <-- ultralight
                                          +-----------------+      smoke
                                                  |
                                          +-------+---------+
                                          | web/ui-demo     |  <-- the HTML
                                          | (overlay HTML)  |      they show
                                          +-----------------+
```

## Build / run cheatsheet

* **Python (uv):** `cd <project> && uv run main.py`
* **C++ (CMake):** `cd <project>` then `.\configure.bat && .\build.bat && .\run.bat` on Windows. The CEF projects auto-download the CEF binary distribution (~250 MB) on first configure; Ultralight expects the SDK installed locally — see each folder's README.
