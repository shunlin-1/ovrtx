# ovrtx-stream — stream any USD scene over WebRTC

ovrtx renders, [ovstream](https://github.com/NVIDIA-Omniverse/ovstream)
transports, neither library knows the other exists. No Kit, no Carbonite, no
NVENC code, no WebRTC code.

## Why this exists

ovstream ships its own `examples/python/ovrtx_stream`, and it is a good
demo — but it streams **one hard-coded scene** that already has a camera and
a RenderProduct authored into it. Point it at a real scene and it renders
nothing but background, or fails outright.

This version takes any scene:

| | stock `ovrtx_stream` | this |
|---|---|---|
| Scene | one hard-coded URL | `--usd <path or URL>` |
| Camera / RenderProduct | must already exist | generated when missing |
| Up-axis | Z-up only | Y-up and Z-up |
| Scene scale | metres assumed | reads `metersPerUnit` |
| Framing | hand-tuned constants | fitted to the world bbox |
| Lighting | scene's own | dome + key added when the scene is unlit |
| Client | open the HTML yourself | `--serve-client` |

That covers this fork's scenes: `agv/` is Y-up in centimetres, the
`commercial-showroom` pack is Z-up in centimetres with no camera, no
RenderProduct, and no lights at all.

## Run

```bash
uv run main.py                                        # stock robot scene
uv run main.py --usd ../../ovrtx/examples/python/agv/Test.usda --serve-client
uv run main.py --usd scene.usd --transport webrtc --transport rtsp:8554
uv run main.py --usd scene.usd --res 2560x1440 --fps 30
```

Then open the browser client and connect to `127.0.0.1:49100`:

- with `--serve-client`: <http://127.0.0.1:8080/index.html>
- otherwise: `../../ovstream/examples/webrtc_client/index.html` straight off
  disk

**Controls:** left-drag orbits, wheel zooms, any key resumes the auto-orbit.
RTSP has no reverse channel, so RTSP clients see the auto-orbit only
(`ffplay rtsp://localhost:8554/stream`).

> First run on a new scene blocks for 1–2 minutes while ovrtx compiles and
> caches shaders. That is an ovrtx startup cost, not a streaming cost, and
> it is paid once.

## Verified

Run end-to-end on 2026-08-18, RTX 5070 Ti (sm_120) / driver 580.173.02,
default stock scene, WebRTC, 1920×1080, RT2 path tracing:

| | |
|---|---|
| First run (cold shader cache) | **>10 min** in the first `renderer.step` |
| Subsequent runs | seconds to first frame |
| Idle, no client attached | ~25 FPS |
| **With a browser client attached** | **~11 FPS** |
| Client connect → first pixels | ~5 s |
| Input round trip (drag → camera moved) | works |

That 25 → 11 drop is the honest cost of NVENC + WebRTC sharing one GPU with a
1080p path trace. If a deployment needs 30 FPS to a client, the lever is
resolution or render mode, not the streaming layer — check with
`--res 1280x720` and `--rendermode "Real-Time"` before assuming ovstream is
the bottleneck.

The cold-start number matters for a 地端 deployment: ship a warm shader cache
or the first launch on a customer machine looks like a hang.

## Measured render cost (`--benchmark`)

`--benchmark N` renders N frames with no streaming server and reports two
numbers: **latency** (output mapped and device synced every frame — what an XR
frame must actually complete) and **pipelined** throughput (one sync at the
end, CPU/GPU overlap allowed). The camera orbits 1°/frame so temporal
accumulation is invalidated every frame, the way head motion does.

RTX 5070 Ti, `RaytracedLighting`, 40 frames after warm-up:

| scene | resolution | Mpx | latency | pipelined |
|---|---|---|---|---|
| single cube | 256×224 | 0.06 | 8.37 ms | **4.63 ms** |
| single cube | 2048×1792 | 3.67 | 17.18 ms | **12.69 ms** |
| AGV (78 MB BIM) | 256×224 | 0.06 | 10.11 ms | — |
| AGV | 1024×896 | 0.92 | 14.24 ms | — |
| AGV | 2048×1792 | 3.67 | 18.12 ms | **14.29 ms** |
| AGV | 3072×2688 | 8.26 | 28.44 ms | — |

Fitting the pipelined numbers:

```
frame ≈ 4.4 ms fixed  +  2.2 ms per Mpx  +  ~1.6 ms for this scene
```

**Three findings worth keeping:**

1. **Scene complexity barely matters.** A single cube costs 12.69 ms at
   2048×1792; the entire 78 MB AGV building costs 14.29 ms. 1.6 ms of
   difference. This inverts the usual game-engine instinct — decimating the
   BIM model, adding LODs, or reducing draw calls buys almost nothing here.
   Resolution is the only lever with real weight.
2. **Render mode has not actually been compared yet.** An earlier run here
   claimed `RaytracedLighting` and `Real-Time Path-Tracing` cost the same
   (18.68 vs 18.48 ms). That was wrong: **`RaytracedLighting` is not an ovrtx
   render mode** — it is an internal Omniverse RTX token that appears only
   inside the RTX plugin binaries, never in ovrtx's API. ovrtx silently falls
   back to the default when it does not recognise the value, so both runs
   rendered RT2 and the "identical" timing was one mode measured twice.
   The three valid values are `Real-Time Path-Tracing` (RT2, default),
   `PathTracing`, and `Minimal`; `--rendermode` now validates against them.
   **`Minimal` — the rasterised mode, and the real analogue of what a game
   engine does for VR — is still unmeasured.**
3. **A per-frame device sync costs ~4.5 ms of pipelining.** If a consumer can
   tolerate one frame of latency, do not map-and-sync inside the render loop.

## How it works

```
scene_probe.py (subprocess, usd-core)
        │  up-axis · metersPerUnit · world bbox · cameras · products · lights
        ▼
   sidecar .usda ──► ovrtx.Renderer.open_usd()
   (only if the scene         │
    has no RenderProduct)     │ step({render_product})
                              ▼
                     LdrColor RGBA8 (CUDA)
                              │ wp.copy + swap_rb kernel → BGRA8
                              ▼
                  ovstream.Server.stream_video()  ──► WebRTC / RTSP / SHM
                              ▲
                              │ on_input: mouse + keyboard
                        browser client
```

Three details that are easy to get wrong and are worth keeping:

**The probe runs in a subprocess.** ovrtx bundles its own USD runtime and
refuses to initialise if `pxr` is already in `sys.modules`. Anything needing
pxr (bbox walks, up-axis lookups) has to live outside the renderer process —
the same split the fork's `agv/pick_collector.py` and
`commercial-showroom/asset_bbox.py` already use.

**The sidecar authors `xformOp:transform`, not `omni:xform`.** `omni:xform` is
ovrtx's *runtime* write slot; USD does not recognise it in `xformOpOrder`, so
authoring a camera through it silently leaves it at identity — at the world
origin, looking down -Z, rendering nothing but the dome. The orbit then
writes `omni:xform` at runtime on top of the authored transform.

**`cuda_context` is not optional.** ovrtx renders in its own CUDA context.
Passing `cuda_device=0` alone makes StreamSDK fail the encode with `CUDA
error invalid argument`; it needs the producer's context handle too:

```python
cfg = ovstream.ServerConfig(width=w, height=h, cuda_device=0,
                            cuda_context=int(wp.get_device("cuda:0").context))
```

## Files

| | |
|---|---|
| `main.py` | the streamer |
| `scene_probe.py` | PEP 723 script, runs under `uv run` with `usd-core` |
| `_generated/` | sidecar stages, regenerated each run — safe to delete |

## Options

```
--usd PATH|URL          scene to render (default: stock Robot-OVRTX)
--transport P[:DETAIL]  webrtc[:port] rtsp[:port] native[:port] shm[:name]
                        cudashm[:name]. Repeatable; all run simultaneously.
--res WxH               default 1920x1080
--fps N                 default 60
--sidecar auto|always|never
--lights auto|always|never
--rendermode NAME       default "Real-Time Path-Tracing"
--render-product PATH   override the RenderProduct prim path
--camera PATH           override the Camera prim path to drive
--serve-client [PORT]   host the bundled WebRTC client (default 8080)
--no-orbit              hold the initial framing
```

## Known limits

- Remote URLs (`https://…`) skip the probe — `usd-core` cannot resolve remote
  asset paths without a resolver plugin — so a remote scene must already
  carry its own camera and RenderProduct. Download it first if it doesn't.
- Stream resolution is fixed at server start; ovstream does not support
  client-driven resize yet. If the scene's authored RenderProduct resolution
  differs from `--res`, the scene wins and the script says so.
- Deprecation warnings from ovrtx 0.4 are expected: this uses standalone
  scene ownership (`open_usd` / `bind_attribute`), matching the rest of the
  fork. NVIDIA's own `ovrtx_stream` example does the same.
