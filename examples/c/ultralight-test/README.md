# ultralight-test — Ultralight HTML-to-PNG sanity check

Smallest possible Ultralight integration: load `content/index.html`,
render it to a CPU bitmap, save as `out.png`, exit. Validates the SDK
download, link, runtime DLL layout, and headless render path before
we commit to wiring Ultralight into a Vulkan window with backdrop
composite.

## Why Ultralight (vs CEF / Slint / RmlUI / PySide6)

Recap of the alternatives we tried, in order:

| Path             | Outcome                                                |
|------------------|--------------------------------------------------------|
| CEF in-process   | Worked, but 18-min DEVICE_LOST and 1500 lines of sync  |
| Slint headless   | Software renderer flattens to RGB → no real alpha      |
| RmlUI scaffold   | Architecturally clean, large authoring effort          |
| PySide6 + QML    | Working, fake glass; real backdrop-blur fragile        |
| **Ultralight**   | WebKit fork, GPUDriver hook, full HTML/CSS/JS subset   |

Ultralight is the closest to "native CSS backdrop-filter blur from a
real browser engine, but inside our own renderer's command stream".
This PoC is *just* the headless rasterizer to confirm the SDK works.

## Build + run

```bash
cd ultralight-test
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
cd build/Release
./ultralight-test.exe
# inspect ./out.png
```

CMake auto-downloads the Windows x64 SDK from Ultralight's CDN
(`ultralight-sdk.sfo2.cdn.digitaloceanspaces.com`) on first configure.

## Expected `out.png`

A 1280×720 image with:

- Dark `#0a0e18` background
- Four blurry coloured orbs at the corners (cyan, purple, peach, mint)
- A centred 480-px glass card with backdrop-blurred gradient title,
  three stat rows, and three buttons (one with cyan→purple gradient).

If the PNG matches that, every piece of the Ultralight rendering path
that matters for our BIM viewer is working.

## Next steps after this PoC

1. **GPUDriver Vulkan backend** — port AppCore's D3D11 driver
   reference impl (open source) to Vulkan. Lets Ultralight emit
   draw calls directly into our existing fragment-shader composite
   pipeline (no CPU readback).
2. **Compose with ovrtx** — sample the Vulkan-backed UI texture
   alongside ovrtx's CUDA-shared scene texture in `fullscreen.frag`.
3. **Input forwarding** — translate GLFW/QWindow mouse + keyboard
   events to `view->FireMouseEvent` / `FireKeyEvent` / `FireScrollEvent`.

License note: free for indie use under the Ultralight free tier
(< $100K revenue, PC platforms, app use). $3K/yr Pro tier removes
the revenue cap and adds full performance + debug binaries.
