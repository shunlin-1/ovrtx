# cef-thread-spike

Standalone spike to validate that CEF (Chromium Embedded Framework) in
multi-threaded message-loop (MTML) mode does not perturb a separate render
thread's frame timing — the threading hypothesis underpinning the
`Omniverse_Kit_WebUI_架構決策.md` plan.

Also doubles as a launchable, browseable CEF window for manually testing CEF
coexistence with `ovrtx` examples (e.g. `neon-robot-c`).

## Why

The architecture doc claims:

> 主執行緒對 CEF 的負擔仍接近 0,因為 CEF MTML 自己跑 thread,主執行緒只在
> dequeue command + apply USD edit 時活躍

This spike measures that claim directly. It also produces a usable interactive
browser binary so you can manually stress-test CEF alongside other GPU-heavy
workloads on the same machine.

## Build

Requires Visual Studio 2022/2026 (with C++ desktop workload) and CMake 3.21+.

```pwsh
.\configure.bat
.\build.bat
```

First configure downloads ~300 MB of CEF binary distribution from
`cef-builds.spotifycdn.com`. First build also compiles `libcef_dll_wrapper`
(~50 source files), so plan for 5–10 minutes. Subsequent builds are fast.

The CEF version is pinned in `CMakeLists.txt`:

```cmake
set(CEF_VERSION "147.0.10+gd58e84d+chromium-147.0.7727.118")
```

If that build is no longer hosted, query the current index:
`curl https://cef-builds.spotifycdn.com/index.json`.

## Run

```pwsh
.\run.bat --mode <baseline|cef-blank|cef-stress|interactive> [options]
```

### Timing modes (--mode baseline | cef-blank | cef-stress)

Spawn a 60 Hz busy-spin "render thread" and measure inter-frame intervals.
Compare distributions across the three modes to see CEF's impact.

| Mode         | What runs                              | Purpose                       |
|--------------|----------------------------------------|-------------------------------|
| `baseline`   | render thread only                     | Establish target distribution |
| `cef-blank`  | render thread + CEF + about:blank      | Idle-CEF MTML overhead        |
| `cef-stress` | render thread + CEF + canvas stress    | Worst-case CEF activity       |

Options:
- `--duration <seconds>` — measurement window (default 10).
- `--csv <path>` — dump per-frame intervals as CSV for offline analysis.

Example sweep:

```pwsh
.\run.bat --mode baseline   --duration 15 --csv build/baseline.csv
.\run.bat --mode cef-blank  --duration 15 --csv build/cef_blank.csv
.\run.bat --mode cef-stress --duration 15 --csv build/cef_stress.csv
```

### Interactive mode (--mode interactive)

Opens a real, browseable Chromium window. Blocks until you close it. Use this
to manually test CEF + neon-robot-c (or any other GPU-heavy app) coexistence.

```pwsh
.\run.bat --mode interactive
.\run.bat --mode interactive --url https://example.com
```

To test alongside `neon-robot-c`:

1. Open two terminals.
2. Terminal 1: launch `neon-robot-c.exe` (the ovrtx example).
3. Terminal 2: `.\run.bat --mode interactive`.
4. Browse heavy pages in CEF while orbiting the camera in `neon-robot-c`.
   Watch for stutters in either window.

## How to read the timing results

The `print_summary()` output lists percentiles:

| Field       | What it means                                       |
|-------------|-----------------------------------------------------|
| `target`    | Ideal frame time (16.667 ms for 60 Hz)              |
| `p50`       | Median frame time                                   |
| `p95`       | 95th percentile (5% of frames are slower than this) |
| `p99` / `p99.9` | Long-tail latency                               |
| `max`       | Single worst frame                                  |
| `> 2x target` | Count of frames worse than 33.3 ms (visible stutter) |

Pass criteria (suggested, tune for your project):
- `p50` delta vs `baseline` < 0.5 ms
- `p99` delta vs `baseline` < 2 ms
- `> 2x target` count == 0

## File layout

```
cef-thread-spike/
├── CMakeLists.txt          # Top-level: fetch CEF, link spike binary
├── configure.bat           # vcvars64 + cmake configure
├── build.bat               # vcvars64 + cmake --build
├── run.bat                 # Launch the built binary with args
├── src/
│   ├── main.cpp            # Entry, mode dispatch, CEF init/shutdown
│   ├── cef_app.{hpp,cpp}   # CefApp / CefBrowserProcessHandler
│   ├── cef_client.{hpp,cpp}# CefClient (Render + Load + LifeSpan handlers)
│   └── render_thread.{hpp,cpp} # 60 Hz busy-spin + percentile reporting
└── test_pages/
    └── stress.html         # JS/canvas stress page (replace stressFrame())
```

## Known caveats

- CEF binary distribution is built `/MT` (static CRT). The spike inherits
  that. If you copy this approach into a project linking libs built with
  `/MD`, you'll hit CRT-mismatch link errors.
- CEF 147 removed the `chrome_runtime` flag — Chrome runtime is the only
  runtime now. Older CEF samples that toggle it will not compile here.
- GPU process crashes when run from a non-interactive sandbox (e.g. CI / some
  remote-shell setups). Run from a normal `cmd` / `pwsh` window for a real
  GPU context.
