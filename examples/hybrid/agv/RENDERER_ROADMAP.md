# Renderer Roadmap

A forward-looking design note for the AGV viewer. **Nothing in here is
work-in-progress** — these are notes captured from a design discussion
about where the renderer could go after ovrtx 0.2.0's limitations
become a blocker.

## Where we are today (ovrtx 0.2.0)

The viewer renders through NVIDIA's `ovrtx` C library, which exposes
four USD `omni:rtx:rendermode` tokens:

| `--rendermode` | What it runs in 0.2.0 | Visual character |
|---|---|---|
| `RaytracedLighting` | **Falls back to Real-Time Path-Tracing** | (will be RTX 2.0 hybrid once shipped) |
| `Real-Time Path-Tracing` | Path tracer with denoiser | Best GI, visible "ghosting" on motion |
| `PathTracing` | Offline-quality path tracer | Cleanest, slowest |
| `Minimal` | Raster preview, no GI | Fastest, flat-looking |

The library outputs exactly two RenderVars: **`LdrColor`** (tone-mapped
sRGB) and **`HdrColor`** (raw HDR pre-tone-map). No depth, normal,
albedo, motion vector, world position, or instance ID AOVs.

### What this prevents

| Effect | Why blocked |
|---|---|
| Toon / cel shading | Needs normals AOV |
| Sobel edge outlines | Needs depth + normal |
| Depth of field / focus blur | Needs depth |
| AR composite with occlusion | Needs depth |
| Wireframe overlay mode | Needs geometry pass |
| Volumetric fog with depth | Needs depth |
| Custom physically-based BRDFs | Closed-box shader pipeline |
| Heatmap by surface property | Needs custom AOV |

ovrtx is closed-source. NVIDIA does not expose a plugin API for custom
render passes, custom AOVs, or custom modes. The four modes above are
the entire surface area available to us.

## Where it can go: USD's Hydra render-delegate architecture

USD itself does not render. It describes scenes. Rendering is done by
a **Hydra render delegate** — a C++ class that translates USD prims
into a renderer's own primitives and produces pixels. ovrtx is just
*one such delegate* shipped as a binary. Multiple delegates can coexist
and be swapped at runtime.

### Delegates that exist today

| Delegate | Ships with | License | Notes |
|---|---|---|---|
| `HdStorm` | OpenUSD core | Apache-2 | Reference OpenGL/Vulkan rasterizer. Used by `usdview`. |
| `HdEmbree` | OpenUSD core | Apache-2 | Reference CPU ray tracer. ~3k LOC. See below. |
| `HdArnold` | SideFX | Apache-2 | Arnold renderer (offline production). |
| `HdRenderMan` | Pixar | Apache-2 | Pixar's RenderMan. |
| `HdRpr` | AMD | Apache-2 | Radeon ProRender (GPU/CPU). |
| `ovrtx` (NVIDIA) | NVIDIA binary | Proprietary | What we use today. |

A custom delegate plugs into the same Hydra interface. If you write
one, your renderer gets to:

- Receive a full USD scene graph (translated to Hydra prims)
- Declare arbitrary AOVs (depth, normal, custom buffers)
- Run any rendering algorithm (cel, NPR, scientific viz, ML inference)
- Composite freely with custom shaders

The same UI / Qt code that talks to ovrtx today can talk to any
delegate — only the renderer backend changes.

## Engines worth considering as the rendering backend

| Engine | License | Strengths | USD adapter? |
|---|---|---|---|
| **Hydra Storm** (in OpenUSD) | Apache-2 | Reference rasterizer. Easiest to fork + add passes. | Native — it *is* a delegate. |
| **HdEmbree** (in OpenUSD) | Apache-2 | Reference path tracer. Best learning material. | Native — it *is* a delegate. |
| **Wicked Engine** | MIT | Modern Vulkan/DX12 + RTX, full G-buffer, FSR/DLSS, custom shader API | None — would need to write a delegate around it. |
| **Filament** (Google) | Apache-2 | Production-grade PBR, mobile-friendly, custom material model | None — would need to write a delegate around it. |
| **bgfx** | BSD-2 | Multi-backend abstraction, tiny, stable | None. |
| **Diligent Engine** | Apache-2 | Multi-backend, more complete than bgfx | None. |
| **Custom Vulkan + OpenUSD C++** | yours | Total control, total cost | n/a — you write it all. |

## What HdEmbree actually is

**Hd** = Hydra (USD's rendering subsystem)
**Embree** = Intel's high-performance CPU ray-tracing library
([github.com/embree/embree](https://github.com/embree/embree)). It's
the CPU equivalent of NVIDIA's OptiX — BVH construction, ray traversal,
intersection kernels, all SIMD-optimized.

**HdEmbree** = the Hydra render delegate that uses Embree to render
USD scenes on the CPU. It lives in OpenUSD's source at
`pxr/imaging/plugin/hdEmbree/` (Apache-2, ~3,000 lines).

### Why HdEmbree is the right reference

- **Small enough to read in a day.** End-to-end USD→pixels in 3k LOC.
- **Reference implementation.** Pixar/Apple maintain it as the canonical
  example of a Hydra delegate. The OpenUSD docs reference it for
  delegate-writing tutorials.
- **All the pieces are visible**: BVH build, ray casting, simple BSDF
  evaluation, AOV output, sampler/integrator, camera, light handling.
- **It actually runs.** Build OpenUSD with HdEmbree, drop it into
  `usdview`, render any USD scene on CPU. No GPU required.

### What it does *not* do (and why that's good for learning)

- No denoiser
- No fancy materials (Lambert + basic GGX only)
- No volumes, displacement, hair
- No GPU acceleration

That minimalism is the point. You see the delegate-pattern skeleton
without 100k lines of production-grade noise around it. The path from
"read HdEmbree" → "write a wireframe delegate" → "write a cel-shading
delegate with G-buffer" is a natural learning curve.

## MDL vs the renderer pipeline (clarifying which layer does what)

A common misconception: *"we can author MDL materials, so we can do
cel shading without changing the renderer."*

Half true. MDL covers the **material** layer beneath cel shading, but
not the **rendering pipeline** or **screen-space post-process** that
cel shading also requires.

### The three layers stacked

```
┌──────────────────────────────────────────────────────────────┐
│ 3. Screen-space post-process    (outlines, edge detect,      │
│                                  color quantize on the       │
│                                  final image)                │
│    ↑ needs G-buffer (depth, normal AOV)                      │
├──────────────────────────────────────────────────────────────┤
│ 2. Rendering pipeline          (integrator, GI / no-GI,     │
│                                  ray budget, denoiser,       │
│                                  AOV outputs)                │
│    ↑ controlled by the delegate / renderer choice            │
├──────────────────────────────────────────────────────────────┤
│ 1. Material response (MDL)     (BSDF / BTDF / emission per   │
│                                  shading point)              │
│    ↑ authored in USD, evaluated by whatever renderer is      │
│      driving the integrator                                  │
└──────────────────────────────────────────────────────────────┘
```

### What MDL alone can do for cel-flavored looks

| Effect | MDL alone? | How |
|---|---|---|
| Flat colors regardless of lighting | ✅ | Emission-only material, no diffuse/specular |
| Quantized diffuse (banding) | ✅ | Custom BSDF that snaps N·L to discrete steps |
| Hard rim "halo" highlight | ✅ | Emission scaled by `(1 − N·V)^k` |
| Texture-mapped flat shading | ✅ | Pure emissive driven by albedo texture |
| Toggleable "cel" mode | ✅ | Swap MDL inputs at runtime (`MaterialOverrides` already does this for neon / xray today) |

### What MDL **cannot** do, no matter how cleverly authored

| Effect | Why MDL can't do it |
|---|---|
| Silhouette outlines | Outlines require detecting where neighboring pixels change depth or normal. MDL evaluates *one shading point at a time* — it has no access to the framebuffer or its neighbors. |
| Hard binary lighting with no GI bleed | Path tracers compute indirect bounces regardless of material. Even with a quantized BRDF, GI smears the bands. Disabling GI is a *renderer* decision, not a material decision. |
| Sobel / edge-detect post-process | Pure screen-space pass over the final image. Belongs at layer 3, not layer 1. |
| View-dependent line thickness | Depends on camera + screen pixel ratio. Needs G-buffer + post-process. |
| Cross-hatch, stipple, sketch | Screen-space stylization. Layer 3. |
| Replace the rendering algorithm itself | MDL is consumed *by* the renderer; it can't *be* the renderer. |

### Practical takeaway for this project

- **"Cel-flavored material"** (flat tints, hard transitions, rim glows
  on a path-traced backdrop): doable today via MDL on existing
  OmniPBR shaders. Add it as a 4th `MaterialOverrides` mode the same
  way `neon` / `xray` / `xray-light` already work. No renderer change.
- **"True cel shading"** (binary lighting + outlines + no GI bleed +
  stylized post-process): needs layers 2 and 3 — which means a custom
  renderer / delegate per the rest of this document. MDL is necessary
  but nowhere near sufficient.

The right way to think about it: **MDL describes *what a surface
does*; the renderer decides *what the picture is*.** Cel shading
wants control over both.

## Migration plan (when this work starts)

The current `AgvBackend` is a thin orchestrator over ovrtx. Adding a
second renderer is purely additive — no existing code needs to be
thrown away.

```
Today:      AgvBackend ─── ovrtx C API ─── rendered RGBA
Future:     AgvBackend ─── EITHER ovrtx OR custom delegate ─── rendered RGBA
```

### Suggested sequence

1. **Read `HdEmbree` source** to internalize the delegate pattern.
   ~1 day of reading. Look at `hdEmbree/renderDelegate.{cpp,h}`,
   `renderPass.cpp`, `renderer.cpp`, `instancer.cpp`.
2. **Build a "hello-world" delegate** that renders flat-shaded geometry
   to a Vulkan window. Validates that USD scene loading + your render
   backend can be wired together. ~1–2 weeks.
3. **Add G-buffer AOVs** (depth, normal, albedo) as RenderVar outputs.
   These are what unlock all the "world-aware" effects above. ~1 week.
4. **Add a cel shader** as a post-process over the G-buffer. This is
   the smallest meaningful "custom render mode" deliverable. ~few days.
5. **Wire it into `AgvBackend`** as a second renderer option. Add a
   `--renderer ovrtx|custom` CLI flag. UI doesn't change.

At step 5 you have:

```bash
./build/agv --renderer ovrtx                    # RTX path tracing
./build/agv --renderer custom --mode cel        # your cel shader
./build/agv --renderer custom --mode wireframe  # your wireframe
./build/agv --renderer custom --mode debug-normals  # G-buffer viz
```

### Estimated effort

| Phase | One engineer, mid-senior | What you have at end |
|---|---|---|
| Read HdEmbree + small experiments | 1 week | Mental model |
| Hello-world delegate | 2 weeks | Triangles on screen |
| G-buffer AOVs | 1 week | Depth + normal available |
| First custom mode (cel or wireframe) | 1 week | Working "custom render mode" button |
| Integrate into AgvBackend | 1 week | Toggle from UI |
| **Total** | **~6 weeks** | A second renderer that does what ovrtx can't |

This is bounded. You're not writing a full renderer — you're writing a
translation layer that hands USD prims to an existing rendering
backend (Wicked / Filament / your own Vulkan).

## Decision triggers

Reasons to start this work:

- Need cel / NPR / wireframe / depth-aware effects ovrtx can't do
- Need to deploy on hardware without NVIDIA GPUs (ovrtx requires CUDA)
- Want to integrate with a non-USD rendering pipeline you already have
- Need a renderer you can profile / modify without waiting on NVIDIA

Reasons to **not** start yet:

- ovrtx 0.2.0's four modes cover your visual needs
- RTX 2.0 hybrid will likely ship in ovrtx 0.3.x and unblock the
  current `RaytracedLighting` fallback
- The current viewer is for R&D iteration, not for shipping a product
- You don't have ~6 weeks of an engineer's time available

## References

- OpenUSD Hydra docs: <https://openusd.org/release/api/_page__hydra__guide.html>
- HdEmbree source: <https://github.com/PixarAnimationStudios/OpenUSD/tree/release/pxr/imaging/plugin/hdEmbree>
- Embree library: <https://github.com/RenderKit/embree>
- Wicked Engine: <https://github.com/turanszkij/WickedEngine>
- Filament: <https://github.com/google/filament>
- bgfx: <https://github.com/bkaradzic/bgfx>
- ovrtx current limitations: see this project's `README.md` "Render modes" section
