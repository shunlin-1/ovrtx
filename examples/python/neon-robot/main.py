#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Neon-lit robot demo: animated SphereLights orbiting the robot-ovrtx scene.

Demonstrates:
- Loading a remote USD scene (the same robot used by examples/c/vulkan-interop)
- Injecting an extra UsdLux light rig via add_usd_layer without touching the source
- Per-frame light-pose updates via bind_attribute/map_attribute (zero-copy)
- GPU-accelerated motion with a Warp kernel

Usage:
    uv run main.py                  # CPU mode, streams to rerun.io
    uv run main.py --gpu            # CUDA mode
    uv run main.py --num-lights 8   # More lights (1-32)
    uv run main.py --png            # Save frames to _output/
    uv run main.py --no-rr          # Disable rerun.io streaming
"""

import argparse
import math
from pathlib import Path

import warp as wp

from ovrtx import Device, PrimMode, Renderer, RendererConfig

SCRIPT_DIR = Path(__file__).parent.resolve()
OUTPUT_DIR = SCRIPT_DIR / "_output"

# Same robot scene used by examples/c/vulkan-interop.
ROBOT_SCENE_URL = (
    "https://omniverse-content-production.s3.us-west-2.amazonaws.com/Samples/"
    "Robot-OVRTX/robot-ovrtx.usda"
)

# Robot scene is in centimeters; light rig dimensions reflect that.
ORBIT_RADIUS = 250.0
ORBIT_HEIGHT = 100.0
BOB_AMPLITUDE = 80.0
LIGHT_RADIUS = 8.0
LIGHT_INTENSITY = 100_000.0

# Saturated neon palette (sRGB). Cycled when num_lights > len(palette).
NEON_COLORS = [
    (1.00, 0.10, 0.55),  # hot pink
    (0.00, 1.00, 1.00),  # cyan
    (0.55, 0.00, 1.00),  # purple
    (0.10, 1.00, 0.30),  # lime
    (1.00, 0.45, 0.00),  # orange
    (0.20, 0.40, 1.00),  # electric blue
    (1.00, 1.00, 0.00),  # yellow
    (1.00, 0.00, 0.00),  # red
]


# [snippet:neon-rig-usda]
def generate_neon_rig_usda(num_lights: int) -> str:
    """Generate a UsdLux SphereLight rig arranged in a ring at orbit height.

    The defaultPrim is "NeonRig"; pass path_prefix="/World/NeonRig" to
    add_usd_layer so the rig hangs off the world stage alongside the robot.
    """
    parts = [
        '#usda 1.0\n(\n    defaultPrim = "NeonRig"\n)\n\n',
        'def Xform "NeonRig" {\n',
    ]
    for i in range(num_lights):
        angle = (math.tau * i) / num_lights
        x = ORBIT_RADIUS * math.cos(angle)
        z = ORBIT_RADIUS * math.sin(angle)
        r, g, b = NEON_COLORS[i % len(NEON_COLORS)]
        parts.append(
            f'''
    def SphereLight "Neon_{i}" (
        prepend apiSchemas = ["ShapingAPI"]
    ) {{
        float inputs:intensity = {LIGHT_INTENSITY}
        float inputs:radius = {LIGHT_RADIUS}
        color3f inputs:color = ({r}, {g}, {b})
        bool inputs:normalize = 1
        matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), (0,0,1,0), ({x:.3f}, {ORBIT_HEIGHT:.3f}, {z:.3f}, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }}'''
        )
    parts.append("\n}\n")
    return "".join(parts)
# [/snippet:neon-rig-usda]


# [snippet:animate-lights-kernel]
@wp.kernel
def animate_lights(
    transforms: wp.array(dtype=wp.mat44d),
    time: wp.float64,
    orbit_speed: wp.float64,
    bob_speed: wp.float64,
    orbit_radius: wp.float64,
    orbit_height: wp.float64,
    bob_amplitude: wp.float64,
    tau: wp.float64,
    num_lights: int,
):
    """Each light traces a ring in the XZ plane while bobbing on Y.

    Phase offsets keep the lights spread evenly around the orbit so the
    ring rotates as a whole rather than clumping.
    """
    tid = wp.tid()
    _0 = wp.float64(0.0)
    _1 = wp.float64(1.0)

    phase = (tau * wp.float64(tid)) / wp.float64(num_lights)
    orbit_angle = time * orbit_speed + phase
    bob_angle = time * bob_speed + phase * wp.float64(2.0)

    x = orbit_radius * wp.cos(orbit_angle)
    z = orbit_radius * wp.sin(orbit_angle)
    y = orbit_height + bob_amplitude * wp.sin(bob_angle)

    transforms[tid] = wp.mat44d(
        _1, _0, _0, _0,
        _0, _1, _0, _0,
        _0, _0, _1, _0,
        x, y, z, _1,
    )
# [/snippet:animate-lights-kernel]


def run_animation(
    device: Device,
    num_lights: int,
    save_png: bool,
    enable_log: bool,
    rr,
) -> None:
    """Drive the renderer and update light poses each simulation step."""
    SIM_HZ = 100
    SIM_DELTA_TIME = 1.0 / SIM_HZ
    SIM_DURATION = 6.0
    NUM_SIM_STEPS = int(SIM_DURATION * SIM_HZ)

    # One full orbit cycle, slow vertical bob (3 cycles over the loop).
    ORBIT_SPEED = math.tau / SIM_DURATION
    BOB_SPEED = 3 * math.tau / SIM_DURATION

    if save_png:
        OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
        from PIL import Image

    log_path = str(OUTPUT_DIR / "neon_robot.log") if enable_log else None
    config = RendererConfig(log_file_path=log_path)
    renderer = Renderer(config)

    print(f"Device: {device.name}")
    print(f"Robot scene: {ROBOT_SCENE_URL}")
    print(f"Lights: {num_lights}, orbit radius={ORBIT_RADIUS} cm, intensity={LIGHT_INTENSITY}")
    print(f"Simulation: {NUM_SIM_STEPS} steps @ {SIM_HZ} Hz ({SIM_DURATION}s)")

    # [snippet:bind-and-animate]
    # 1. Load the remote robot scene.
    renderer.add_usd(ROBOT_SCENE_URL)

    # 2. Inject the neon rig as a sublayer rooted under /World/NeonRig.
    rig_usda = generate_neon_rig_usda(num_lights)
    renderer.add_usd_layer(rig_usda, path_prefix="/World/NeonRig")

    # 3. Bind omni:xform on every light for zero-copy per-frame updates.
    light_paths = [f"/World/NeonRig/Neon_{i}" for i in range(num_lights)]
    light_binding = renderer.bind_attribute(
        prim_paths=light_paths,
        attribute_name="omni:xform",
        dtype="float64",
        shape=(4, 4),
        prim_mode=PrimMode.MUST_EXIST,
    )

    cuda_stream = None
    rendered_frame_count = 0
    for sim_step in range(NUM_SIM_STEPS):
        sim_time = sim_step * SIM_DELTA_TIME

        # Map -> compute new transforms on device -> unmap (flush to Fabric).
        with light_binding.map(device=device, device_id=0) as attr_mapping:
            wp_transforms = wp.from_dlpack(attr_mapping.tensor, dtype=wp.mat44d)
            if cuda_stream is None and device == Device.CUDA:
                cuda_stream = wp.Stream(device=wp_transforms.device)

            wp.launch(
                kernel=animate_lights,
                dim=num_lights,
                inputs=[
                    wp_transforms,
                    wp.float64(sim_time),
                    wp.float64(ORBIT_SPEED),
                    wp.float64(BOB_SPEED),
                    wp.float64(ORBIT_RADIUS),
                    wp.float64(ORBIT_HEIGHT),
                    wp.float64(BOB_AMPLITUDE),
                    wp.float64(math.tau),
                    num_lights,
                ],
                device=wp_transforms.device,
            )
            attr_mapping.unmap(stream=cuda_stream.cuda_stream if cuda_stream else None)

        products = renderer.step(
            render_products={"/Render/Camera"},
            delta_time=SIM_DELTA_TIME,
        )

        for product_name, product in products.items():
            for frame in product.frames:
                rendered_frame_count += 1
                if "LdrColor" not in frame.render_vars:
                    continue
                with frame.render_vars["LdrColor"].map(device=device) as mapping:
                    np_array = wp.from_dlpack(mapping.tensor).numpy()
                    if save_png:
                        Image.fromarray(np_array).save(
                            OUTPUT_DIR / f"neon_robot_{rendered_frame_count:03d}.png"
                        )
                    if rr:
                        rr.set_time("sim_time", duration=sim_time)
                        rr.log("render/LdrColor", rr.Image(np_array))

    light_binding.unbind()
    # [/snippet:bind-and-animate]

    print(
        f"Done. {NUM_SIM_STEPS} sim steps -> {rendered_frame_count} rendered frames."
    )
    if save_png:
        print(f"  Frames saved to: {OUTPUT_DIR}")


def main():
    parser = argparse.ArgumentParser(
        description="Neon-lit robot demo using ovrtx Python bindings.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  uv run main.py                  # CPU, streams to rerun.io
  uv run main.py --gpu            # CUDA
  uv run main.py --num-lights 12  # 12 neon lights orbiting
  uv run main.py --png            # Save frames to _output/
  uv run main.py --no-rr          # Disable rerun.io streaming

To create a video from frames:
  ffmpeg -framerate 60 -i _output/neon_robot_%03d.png -c:v libx264 -pix_fmt yuv420p neon_robot.mp4
""",
    )
    parser.add_argument("--gpu", action="store_true", help="Use CUDA device (default: CPU)")
    parser.add_argument("--num-lights", type=int, default=6, help="Number of neon lights, 1-32 (default: 6)")
    parser.add_argument("--png", action="store_true", help="Save rendered frames as PNGs to _output/")
    parser.add_argument("--no-rr", action="store_true", help="Disable rerun.io streaming (enabled by default)")
    parser.add_argument("--log", action="store_true", help="Enable carb log file in _output/")
    args = parser.parse_args()

    num_lights = max(1, min(32, args.num_lights))

    rr = None
    if not args.no_rr:
        try:
            import rerun as rr

            rr.init("ovrtx_example_neon_robot", spawn=True)
        except ImportError:
            print("Warning: rerun not installed, skipping rerun.io streaming.")
            print("  Install with: pip install rerun-sdk")

    wp.init()
    run_animation(
        device=Device.CUDA if args.gpu else Device.CPU,
        num_lights=num_lights,
        save_png=args.png,
        enable_log=args.log,
        rr=rr,
    )


if __name__ == "__main__":
    main()
