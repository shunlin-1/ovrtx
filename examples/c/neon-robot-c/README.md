# neon-robot-c

Live, interactive Vulkan-window viewer for the `robot-ovrtx` scene with a rig of animated neon `SphereLight`s orbiting the robot. Forked from `vulkan-interop` — same CUDA-Vulkan interop pipeline, mouse-orbit camera, and per-frame timing prints — with two additions:

1. The scene is opened with `ovrtx_open_usd_from_file` and supplies six saturated `SphereLight`s at `/World/Lights/Neon_<i>`; the app only mutates their transforms.
2. Each render step, the lights' `omni:xform` transforms are recomputed (orbital path + vertical bob) and pushed via `ovrtx_set_xform_mat` — a synchronous CPU-side write, fast enough for a handful of lights.

Mouse-drag to orbit the camera. Scroll to zoom. The lights keep moving regardless of camera input.

## Linux

### Prerequisites

- `sudo apt install build-essential cmake`
- [Vulkan SDK 1.3.250+](https://vulkan.lunarg.com/sdk/home)
- [CUDA Toolkit 12.0+](https://developer.nvidia.com/cuda-downloads)

If ovrtx or glfw3 are already installed and available via `CMAKE_PREFIX_PATH`, the local installations are used. Otherwise they are downloaded automatically at configure time. Other dependencies (GLM, volk, unordered_dense) are always downloaded via FetchContent.

### Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Running

```bash
./build/neon-robot-c
```

## Windows

### Prerequisites

- [Visual Studio 2017+](https://visualstudio.microsoft.com/downloads/)
- [Vulkan SDK 1.3.250+](https://vulkan.lunarg.com/sdk/home)
- [CUDA Toolkit 12.0+](https://developer.nvidia.com/cuda-downloads)

If ovrtx or glfw3 are already installed and available via `CMAKE_PREFIX_PATH`, the local installations are used. Otherwise they are downloaded automatically at configure time. Other dependencies (GLM, volk, unordered_dense) are always downloaded via FetchContent.

### Building

```pwsh
cmake -B build
cmake --build --config Release
```

### Running

```pwsh
.\build\Release\neon-robot-c.exe
```

The example is configured to load the robot scene from Omniverse:

| Setting | Value |
|---------|-------|
| USD Scene | `https://omniverse-content-production.s3.us-west-2.amazonaws.com/Samples/Robot-OVRTX/robot-ovrtx.usda` |
| Render Product | `/Render/Camera` |
| Up Axis | Z |
| Units | Meters |

### Controls

- **Left-click and drag** — Rotate camera around the target point
- **Mouse wheel** — Dolly camera in/out

# Licensing

This example contains stb_image_write.h, © Sean Barrett, released under Public Domain.
