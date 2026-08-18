# XR with ovrtx — how it actually works, and what it would take here

**Status on this machine: scaffold only.** There is no OpenXR runtime and no
headset here, so nothing in this folder can be *run* past `openxr-stub`.
That is a hardware gap, not a design gap — the API path is clear and is
documented below.

---

## The one thing to understand

Everyone expects the XR integration to look like "render stereo, encode it,
send it somewhere." NVIDIA's design inverts that:

> **Your application is a plain OpenXR app. The streamer *is* the OpenXR
> runtime.**

You never write encoding, never write WebRTC, never touch a headset SDK. You
call `xrCreateSession` / `xrLocateViews` / `xrEndFrame` as if a headset were
plugged into the machine. The CloudXR Runtime registers itself as the
system's OpenXR runtime, takes your submitted swapchain images, encodes them
with NVENC, and streams them to a real device somewhere else.

This is verifiable in the Omniverse install already on this box:

```
IsaacSim/_build/linux-x86_64/release/extscache/omni.kit.xr.system.openxr-107.3.109+.../bin/
├── libopenxr_loader.so.1.1.45      standard Khronos loader
├── openxr_cloudxr.json             runtime manifest:
│                                     "NVIDIA™ CloudXR™ Runtime (based on Monado™)"
├── libopenxr_cloudxr.so            ← the runtime that manifest points at
├── libcloudxr.so
├── libNvStreamServer.so            ← same streaming core as the WebRTC livestream ext
└── libNvStreamBase.so
```

Kit is not special here. `omni.kit.xr.system.openxr` calls exactly the
functions any OpenXR app calls — `xrCreateVulkanDeviceKHR`,
`xrCreateSwapchain`, `xrAcquireSwapchainImage`, `xrLocateViews`,
`xrEndFrame` — plus a foveation-aware swapchain manager on top. An ovrtx app
would sit in precisely the same seat.

Note the shared bottom layer: `libNvStreamServer.so` is the same library
behind `omni.kit.livestream.webrtc` and behind the public
[`ovstream`](https://github.com/NVIDIA-Omniverse/ovstream) package. XR and
2D WebRTC streaming are one transport wearing two faces.

---

## The two halves

### Server: CloudXR Runtime + your ovrtx app

| Piece | Who writes it |
|---|---|
| OpenXR session, swapchains, frame loop | **you** |
| Two cameras + two RenderProducts in USD, one per eye | **you** |
| Per-eye pose → camera transform each frame | **you** |
| Stereo encode, network transport, pose feedback | CloudXR Runtime |

### Client: CloudXR.js in a browser

`@nvidia/cloudxr` **6.2.0**, public on npm, vendored here under
[`cloudxr-js/`](cloudxr-js/). It is a WebXR client — the browser opens an
`XRSession`, and CloudXR.js composites the decoded remote frames into the
WebXR framebuffer.

The whole client integration is three calls:

```js
import { createSession } from '@nvidia/cloudxr';

const session = createSession({
  serverAddress: '192.168.1.100',
  serverPort: 49100,
  // perEyeResolution must be a multiple of 16 in both dimensions
});

session.connect();

// inside the WebXR animation loop:
session.sendTrackingStateToServer(time, frame);   // pose + controllers up
session.render(time, frame, xrWebGLLayer);        // decoded frame down
```

Transport is a WebSocket for signaling plus WebRTC for media — the same
handshake family as ovstream's WebRTC transport. Client support is focused on
Meta Quest and Pico class devices; desktop browsers work for development.

Full API: `cloudxr-js/package/build/Session.d.ts` (31 KB of typed, documented
surface) and the [CloudXR.js user guide](https://docs.nvidia.com/cloudxr-sdk/latest/usr_guide/cloudxr_js/index.html).

---

## Probe result: the bundled runtime is half a runtime

IsaacSim ships a CloudXR OpenXR runtime — `libopenxr_cloudxr.so`, version
string **5.0.1**, exporting `xrNegotiateLoaderRuntimeInterface`, so the
Khronos loader will load it. It is registered on this machine at:

```
~/.config/openxr/1/active_runtime.json     ← remove this file to unregister
```

With it registered, `openxr_stub` gets **much** further — the runtime loads
and enumerates its extension list:

```
XR_KHR_vulkan_enable2 (v2)                          ← what an ovrtx app needs
XR_KHR_composition_layer_depth (v6)
XR_EXT_hand_tracking (v4)
XR_NVX1_foveation_piecewise_quadratic_warp (v1)     ← NVIDIA's foveation path
XR_NVX1_opaque_data_transport (v1)
XR_NVX1_override_scaling (v1)
```

Then `xrCreateInstance` fails with `XR_ERROR_RUNTIME_FAILURE`:

```
ERROR [ipc_client_socket_connect] Failed to connect to socket
      /run/user/1000/ipc_cloudxr: No such file or directory!
ERROR [ipc_client_connection_init] Failed to connect to monado service process
```

**CloudXR is Monado-based and split in two**: a client-side `.so` that apps
load, and a **service daemon** that owns the IPC socket and does the actual
rendering handoff, encoding and streaming. Kit ships only the client half.
Searching this machine — IsaacSim, the Kit app templates, `/opt/nvidia`,
`~/.local/share/ov` — finds **no `monado-service` and no runtime daemon**.

So the bundled files cannot stream on their own. The daemon comes from the
**CloudXR Runtime package on NGC**, which is the supported install anyway.

> ⚠️ When you do install CloudXR Runtime 6.x, it will register its own
> `active_runtime.json`. Delete the 5.0.1 entry above first, or the stale
> pointer wins and 6.x looks broken.

Useful side effect: those `XR_NVX1_*` extension names confirm the foveation
story — piecewise quadratic warp, override scaling, opaque data transport are
exactly the levers Kit's XR uses to make a heavy renderer hit headset frame
rates.

## The runtime daemon: `nvcr.io/nvidia/cloudxr-runtime:5.0.2`

Pulled and inspected on this machine (440 MB). The container **is** the missing
daemon half.

Available tags are only `0.1.0-isaac`, `5.0.0`, `5.0.1`, `5.0.2` — there is no
6.x container; CloudXR 6.x ships as the `CloudXR-<version>-Linux-sdk.tar.gz`
resource on NGC instead. 5.0.2 matches the 5.0.1 client shim IsaacSim ships,
so container and client are the same train.

How it is meant to be wired, from its own `/entrypoint.sh`:

```
Entrypoint  /entrypoint.sh /opt/nvidia/cloudxr/bin/cloudxr-service
XDG_RUNTIME_DIR    /openxr/run
XR_RUNTIME_JSON    /openxr/share/openxr/1/openxr_cloudxr.json
```

On start it populates the `/openxr` mount for the host to consume:

| It writes | Host app uses it for |
|---|---|
| `/openxr/lib/libopenxr_cloudxr.so` | the client shim your app loads (5.0.2) |
| `/openxr/share/openxr/1/openxr_cloudxr.json` | runtime manifest, relative `library_path` |
| `/openxr/run/ipc_cloudxr` | the IPC socket the shim connects to |

So the host side needs no install at all — mount a directory, and the daemon
hands you the client library and socket through it. Container uid/gid is
`1000:1000`, same as this host, so the bind mount needs no permission fixing.

**It will not start without accepting the EULA** (`/eula.sh` gates the
entrypoint): pass `-e ACCEPT_EULA=Y`. Terms:
<https://developer.download.nvidia.com/cloudxr/EULA/NVIDIA_CloudXR_GA_License_without_Data_Collection_25Feb2025.pdf>

```bash
mkdir -p ~/openxr
docker run -d --name cloudxr --gpus all --network host \
    -e ACCEPT_EULA=Y \
    -v ~/openxr:/openxr \
    nvcr.io/nvidia/cloudxr-runtime:5.0.2

# then, against the container-provided 5.0.2 shim rather than IsaacSim's 5.0.1:
cd examples/c/openxr-stub
XDG_RUNTIME_DIR=$HOME/openxr/run \
XR_RUNTIME_JSON=$HOME/openxr/share/openxr/1/openxr_cloudxr.json \
    ./build/openxr_stub
```

`XR_RUNTIME_JSON` overrides the loader's global lookup, so this needs no
`~/.config/openxr/1/active_runtime.json` at all — prefer it, and delete the
5.0.1 registration to avoid two runtimes fighting.

## `examples/c/openxr-stub/` — what builds and runs today

```bash
cd examples/c/openxr-stub
cmake -B build -DCMAKE_BUILD_TYPE=Release   # fetches OpenXR-SDK 1.1.43
cmake --build build -j8
./build/openxr_stub
```

Verified on this machine: **builds clean, runs, and reports**

```
xrCreateInstance failed: XR_ERROR_RUNTIME_UNAVAILABLE
```

which is the correct answer when no runtime is installed. With a runtime
present it goes on to print the runtime name and the per-eye recommended
render target size — the number that decides whether the whole idea is
affordable.

One build note worth keeping: the OpenXR SDK turns on `XR_USE_PLATFORM_XCB`
whenever pkg-config sees the `xcb` module, then fails on `<xcb/glx.h>`, which
lives in `libxcb-glx0-dev`. `CMakeLists.txt` sets `BUILD_WITH_XCB_HEADERS OFF`
instead of adding an apt dependency — an ovrtx XR app is Vulkan-only and never
touches the GLX structs.

---

## The frame loop, concretely

```
xrWaitFrame                        runtime tells you when to render for
                                     predictedDisplayTime
xrBeginFrame
xrLocateViews(predictedDisplayTime)  → per-eye pose + asymmetric FoV
  write eye 0 pose  → /World/CameraL  omni:xform
  write eye 1 pose  → /World/CameraR  omni:xform
  (asymmetric FoV → per-eye aperture + aperture offset)
renderer.step({productL, productR})  ovrtx renders both eyes
  map LdrColor (CUDA/Vulkan) → blit into acquired swapchain images
xrEndFrame(XrCompositionLayerProjection)
```

Every step maps onto an API that already exists in ovrtx 0.4. Nothing here
needs a feature request.

## The frame budget — measured, not guessed

Benchmarked on the RTX 5070 Ti with `examples/python/ovrtx-stream --benchmark` (see its
README for the full table). Pipelined cost fits:

```
frame ≈ 4.4 ms fixed + 2.2 ms/Mpx + ~1.6 ms scene
```

Per eye at 2048×1792 (the resolution CloudXR.js's own example uses) that is
**14.3 ms**. Stereo therefore lands between:

| | frame | rate |
|---|---|---|
| best case (both eyes in one `step()`, sharing the fixed cost) | 24.2 ms | 41 FPS |
| worst case (two separate steps) | 28.6 ms | 35 FPS |

So against a 72 Hz headset the gap is **~1.8×**, against 90 Hz **~2.2×**.
Not the order of magnitude an accumulating path tracer suggests.

What closes ~2×:

- **Lower per-eye resolution.** 1440×1584 instead of 2048×1792 is 38% fewer
  pixels → roughly 17.8 ms stereo → ~56 FPS. Most of the gap, from one number.
- **Foveated rendering.** Typically 30–50% effective pixel reduction — and
  the runtime already advertises it as
  `XR_NVX1_foveation_piecewise_quadratic_warp` / `XR_NVX1_override_scaling`.
- **CloudXR reprojection** for whatever remains.

**Revised verdict: worth pursuing.** The earlier "park it" call was based on
extrapolating from a throttled, encode-inclusive streaming loop, which
overstated the gap by roughly 4×. The honest blocker is now hardware
(a headset and the CloudXR daemon), not physics.

The one measurement still missing, and the cheapest one left: whether
`renderer.step()` with **two** render products shares the 4.4 ms fixed cost or
pays it twice. That decides 41 FPS vs 35 FPS and needs no headset — just a
two-camera sidecar.

## What is still genuinely hard

ovrtx's RT2 mode is a **path tracer that accumulates samples across frames**.
The fork's own guidance (`ovrtx/skills/warmup/SKILL.md`) is ~40 warmup frames
for texture streaming and convergence. In XR, head motion invalidates that
accumulation *every frame*, at 90 Hz, twice — once per eye.

So the real questions, in order:

1. What does RT2 converge to in a **single** frame at per-eye resolution?
2. Does DLSS / the denoiser carry it to acceptable at 90 Hz?
3. Does CloudXR's reprojection cover the remaining latency?

Kit's XR stack answers these with a foveation pipeline (`quadview`,
warped insets, `foveation/dimFactor` — all visible as settings strings in
`libomni.ext-system.openxr.plugin.so`). That layer is a project in itself,
and it is the reason to treat "ovrtx in XR" as a program rather than an
extension.

**Recommendation: park this until a headset exists.** Nothing is blocked
architecturally. But the answer to "is it good enough" is a measurement, and
this machine cannot take it.

---

## Getting unblocked, in order of effort

1. **Monado + simulated HMD** — an OpenXR runtime with a fake headset driver.
   Turns `openxr-stub` into a real running frame loop with no hardware. Best
   first step for validating the ovrtx-side plumbing.
2. **CloudXR Runtime** from NGC (login-gated download) + a Quest/Pico on the
   same WiFi 6 network. This is the real thing. Requirements from NVIDIA:
   <20 ms latency, 100+ Mbps, per-eye resolution a multiple of 16.
3. **CloudXR.js sample client** — already vendored in `cloudxr-js/`; needs a
   server to point at.

## Licensing

`@nvidia/cloudxr` ships under the NVIDIA CloudXR License
(`cloudxr-js/package/LICENSE`). Read it before it goes into a customer
delivery — same due-diligence note as ovstream's NVIDIA SLA + Omniverse
product terms.
