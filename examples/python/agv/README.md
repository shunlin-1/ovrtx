# agv — AGV viewer (ovrtx + PySide6)

Loads `Test.usda` (sitting next to `main.py`), renders with ovrtx
(`omni:rtx:rendermode = "RaytracedLighting"` requested — ovrtx 0.2.0
will warn and fall back to Real-Time Path-Tracing if not yet
implemented), and shows the result in a PySide6 + QML window with
right-click orbit, wheel zoom, and click-to-toggle material modes.

## Architecture

* **main.py** — generates a sidecar `agv_render.usda` that references
  `Test.usda` and adds `/World/Camera` plus `/Render/Camera` with the
  requested RTX render mode. Drives ovrtx on a worker thread so the
  UI loop stays at native Qt rate.
* **pick_collector.py** — stand-alone helper that walks the USD with
  `pxr` to build a pick table (per-mesh AABB + triangulated world-space
  vertices + bound MDL shader). Runs in its own ephemeral venv via
  `uv run` + PEP 723 inline metadata, because `ovrtx` refuses to load
  when `usd-core` is installed in the same env (it bundles its own
  USD libs). Output is exchanged as a pickle file.
* **qml/Main.qml** — minimal AGV viewer chrome: backdrop + frame-count
  pill + 3-button neumorph toolbar (Neon / Xray / Xray-Light).

## Run

```powershell
cd <repo>/examples/python/agv
uv run main.py
```

Override the USD path or render mode:

```powershell
uv run main.py --usd "C:\path\to\some.usda" --rendermode RaytracedLighting
```

Available `--rendermode` tokens (USD authored, ovrtx may fall back):

* `RaytracedLighting` — RTX 2.0 hybrid raytracing **(default,
  recommended for live viewport — no path-tracer ghosting)**
* `Real-Time Path-Tracing` — what ovrtx 0.2.0 actually runs today
* `PathTracing` — full quality, slow
* `Minimal` — lightweight raster preview, no GI

## Controls

* **Right-button drag** — orbit camera
* **Mouse wheel** — dolly / zoom
* **Left click on a mesh** — apply the currently-selected toolbar
  material override; click the same mesh again to restore its
  original OmniPBR inputs.

## Material modes

| Mode | Effect on the clicked mesh |
|---|---|
| **Neon** | Opaque body + strong cyan emission (`emissive_intensity` 10000) — full glow |
| **Xray** | Translucent (`opacity_constant` 0.15), no emission — ghost / glass |
| **Xray-Light** | Translucent (0.40) + soft cyan emission (1500) — hologram look |

## Notes

* `Test.usda` is the sample AGV scene shipped with this example
  (Y-up, `metersPerUnit = 0.01`). Bring your own with `--usd`.
* `agv_render.usda` and `agv_render.picks.pkl` are *generated at
  runtime*; they are gitignored. Both are recreated each launch
  against the current USD.
