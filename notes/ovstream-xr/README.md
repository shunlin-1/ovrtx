> **These are notes, not code.** They were written while spiking ovstream
> (WebRTC streaming) and CloudXR (XR) against this fork on 2026-08-18. The
> working demo they describe lives outside this repo, in `~/Desktop/ovlibs/`
> on the dev machine — `demo/ovrtx-stream/{main.py,scene_probe.py}` — and is
> **not committed here**. Everything below is reproducible from the commands
> in these files.

# ovlibs — NVIDIA Omniverse libraries workspace

One folder wrapping the ov* libraries this fork builds on, plus the demos
that compose them.

```
ovlibs/
├── ovrtx/                 your fork (moved here; ~/Desktop/ovrtx is now a symlink)
├── ovstream/              NVIDIA-Omniverse/ovstream — WebRTC/RTSP/SHM streaming
├── demo/ovrtx-stream/     stream any USD scene over WebRTC        ← the demo
└── xr/                    CloudXR / OpenXR scaffold + findings    ← scaffold only
```

## Why this shape

The 2026-05-08 architecture decision (`ovrtx/Omniverse_Kit_WebUI_架構決策.md`)
bet on **ov\* libraries instead of Kit**. That bet has since been confirmed by
NVIDIA's own packaging: streaming, which used to be reachable only as the Kit
extension `omni.kit.livestream.webrtc`, now ships as a standalone library with
no Kit and no Carbonite dependency.

Two open items in that doc are now answered:

| Doc line | Question | Answer |
|---|---|---|
| 553 | "有沒有內建 H.264 / WebRTC encode,或要自己接 NVENC SDK" | **內建.** ovstream does NVENC + WebRTC. No encoder code to write. |
| 387 | "待 spike 確認 ovrtx 是否內建 streaming" | ovrtx does not — ovstream does, as a separate library, by design. |

## `ovstream` — the streaming half

`pip install ovstream`, or CMake `ovstream_fetch()` (mirrors `ovrtx_fetch()`).

- Frames go in as **CUDA device pointers** (BGRA8), encoded on-GPU with NVENC.
  Also DLPack tensors, or pre-encoded H.264/H.265/AV1 passthrough.
- Five transports: **WebRTC, RTSP, native, SHM, CUDASHM** — selectable at
  runtime, several at once.
- Input comes back: keyboard, mouse, Unicode, plus bidirectional messaging.
- Ships `examples/webrtc_client/` — a browser client with no build step.

Note: off-the-shelf WebRTC tooling will **not** interoperate. The client has
to speak StreamSDK's signaling flavor, which is what the bundled JS library
implements.

## `demo/ovrtx-stream` — the demo

Streams any USD scene, generating the camera + RenderProduct + lights when the
scene has none. See its [README](ovrtx-stream-demo.md).

**Verified working**: browser client connected over WebRTC, path-traced frames
displayed, left-drag orbit driving the ovrtx camera. ~25 FPS idle / ~11 FPS
with a client attached at 1920×1080 RT2 on the 5070 Ti.

```bash
cd demo/ovrtx-stream
uv run main.py --serve-client                    # stock scene
uv run main.py --usd ../../ovrtx/examples/python/agv/Test.usda --serve-client
```

## `xr/` — scaffold only

No headset and no OpenXR runtime on this machine, so the XR half is documented
and scaffolded, not running. The key finding is in its
[README](cloudxr-and-openxr.md): **the streamer is the OpenXR runtime.** Your app is a
plain OpenXR app; NVIDIA's CloudXR Runtime registers itself as the system
runtime, takes your stereo swapchain, and streams it. Kit is not special —
`omni.kit.xr.system.openxr` is just an OpenXR client, and it ships
`libNvStreamServer.so`, the same streaming core as ovstream.

`xr/openxr-stub/` builds and runs; it reports `XR_ERROR_RUNTIME_UNAVAILABLE`,
which is correct here. `xr/cloudxr-js/` has `@nvidia/cloudxr` 6.2.0 from npm.

## Note on the moved checkout

`~/Desktop/ovrtx` is now a symlink to `ovlibs/ovrtx`, so existing venvs, shell
history and IDE bookmarks keep working. To finish the move cleanly:

```bash
rm ~/Desktop/ovrtx      # removes only the symlink; the repo is in ovlibs/
```

Then re-run `uv sync` in any example whose venv still has the old absolute
path baked in (`examples/python/office-gs-dataset/train/.venv` is the big one).

## Licensing

Both ovrtx and ovstream are pre-release under the NVIDIA Software License
Agreement + Product-Specific Terms for Omniverse; `@nvidia/cloudxr` is under
the NVIDIA CloudXR License. Worth reading before any of it goes into a
地端 government delivery.
