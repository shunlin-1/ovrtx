# neon-robot-bim — PySide6 + ovrtx BIM-style viewer demo

Stage-1 prototype: Qt/QML-led desktop UI with overlay panels (glass,
neumorphic, drag, hover). Stage 2 will swap the placeholder dark
gradient backdrop for live ovrtx-rendered scene frames.

## Why Qt-led

See `../../c/neon-robot-c/SLINT_LESSONS.md` for the full decision log.
Short version:

- **Slint software renderer** flattens to RGB; chroma-key alpha can't
  reproduce backdrop-blur compositing on 3D scene → walked away
  after wiring everything up.
- **RmlUI** scaffolded successfully (still in `examples/c/neon-robot-c`
  behind `-DENABLE_RMLUI=ON`) but RML/RCSS authoring + manual Vulkan
  RenderInterface looked like 1+ week of work for what Qt 6 gives
  natively.
- **PySide6 + Qt 6** is the industry-standard architecture for
  BIM/DCC-style desktop apps. Maya, Houdini, FreeCAD, KiCad all use
  Qt for chrome + embedded 3D viewport. `MultiEffect` provides the
  CSS-`backdrop-filter`-equivalent at the OS-compositor level.

## Stage 1 — running this

```bash
# from this folder
uv sync          # installs PySide6 + ovrtx (ovrtx not used yet in S1)
uv run main.py   # opens Qt window with overlay UI demo
```

Expected: a 1600×1000 window with a dark gradient backdrop, six
"neon orb" placeholders (Stage-2 replacement for the actual 3D
scene), and four overlay UI clusters:

| Position     | Element                   |
|--------------|---------------------------|
| Top-center   | 3 hover-glow buttons      |
| Top-right    | Pulsing "Live" indicator  |
| Top-left     | Draggable Selection card  |
| Bottom-left  | 3 neumorphic tool buttons |
| Bottom-right | Glassmorphism stats panel |

## Stage 2 — TODO

- `main.py` instantiates `ovrtx.Renderer`, loads a USD scene
- Per Qt frame tick: `renderer.step(...)` → numpy RGBA → `QImage`
- Bind that QImage to a QML `Image { source: ... }` element behind
  the overlay UI
- Forward Qt mouse events back into the orbit camera attribute
  on the ovrtx side

## Files

- `pyproject.toml` — deps (PySide6, ovrtx, numpy)
- `main.py` — entry point (loads QML)
- `qml/Main.qml` — UI definition
