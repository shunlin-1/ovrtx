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
| [`c/ultralight-test/`](c/ultralight-test/) | Ultralight | Smallest possible Ultralight integration: HTML → CPU bitmap → PNG. Smoke-test for SDK layout and headless render path. |

## Web

| Project | What it is |
|---|---|
| [`web/ui-demo/`](web/ui-demo/) | Single-file HTML exercising glassmorphism / neumorphism / hover / drag on a `transparent` background — the test page the CEF / Ultralight integrations are designed to display on top of an ovrtx scene. |

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
