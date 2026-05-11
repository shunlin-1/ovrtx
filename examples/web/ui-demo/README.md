# ui-demo — HTML UI effects on a transparent background

Single self-contained `index.html` that exercises the visual effects
we want to overlay on a 3D scene rendered by ovrtx:

* **Glassmorphism** — bottom-right info panel with `backdrop-filter:
  blur()` so the scene behind shows through.
* **Neumorphism** — soft-shadow inset buttons.
* **Hover** — animated state transitions.
* **Drag** — pointer-event-driven panel drag.

The body background is `transparent`, so this page is meant to be
loaded by a host that paints the 3D scene first (CEF off-screen render
to a Vulkan texture, Ultralight bitmap composited into a swap-chain
image, etc.) and then composites this HTML on top.

## Run standalone (browser)

Just open it. There's no build step; it has no JS dependencies.

```powershell
start examples\web\ui-demo\index.html
```

You'll see the panel rendered against the browser's default white
background — the glassmorphism blur is most visible when overlaid on a
non-trivial scene (see the CEF / Ultralight integration tests under
`examples/c/`).

## Used by

* `examples/c/cef-bim-test/` — loads this kind of layout into CEF
  off-screen, composites with ovrtx Vulkan output.
* `examples/c/ultralight-test/` — Ultralight equivalent.
