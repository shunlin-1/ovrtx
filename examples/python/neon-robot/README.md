# Neon Robot

Animated neon-light demo using ovrtx Python bindings. Loads the same `robot-ovrtx.usda` scene used by `examples/c/vulkan-interop`, then injects a rig of `UsdLux` `SphereLight`s in saturated neon colors that orbit the robot and bob vertically. Light poses are updated per simulation step via `bind_attribute`/`map_attribute` with a Warp kernel — no changes to the source robot scene.

By default, rendered frames are streamed to [rerun.io](https://rerun.io/) for live visualization. Frames can also be saved to disk as PNGs.

## Prerequisites

- Python 3.10–3.13
- [uv](https://docs.astral.sh/uv/)
- NVIDIA GPU + driver (and the Vulkan loader, which the driver normally pulls in)
- Network access on first run (the robot USD is fetched from S3)

## Running

```bash
uv run main.py
```

### Options

| Flag | Description |
|------|-------------|
| `--gpu` | Run the Warp kernel on CUDA (default: CPU) |
| `--num-lights N` | Number of neon SphereLights, 1–32 (default: 6) |
| `--png` | Save rendered frames as PNGs to `_output/` |
| `--no-rr` | Disable rerun.io streaming |
| `--log` | Enable carb log file in `_output/` |

The first time the example is run, driver shader compilation will be performed and cached. Subsequent runs will be much faster.

## How it works

1. `renderer.add_usd(ROBOT_SCENE_URL)` loads the robot scene from S3.
2. `generate_neon_rig_usda()` builds an in-memory USDA layer containing `N` `SphereLight` prims arranged in a ring at waist height. Each light gets a saturated neon color from a fixed palette.
3. `renderer.add_usd_layer(rig_usda, path_prefix="/World/NeonRig")` injects the rig into the live stage without modifying the source.
4. `renderer.bind_attribute(..., attribute_name="omni:xform", shape=(4,4), dtype="float64")` opens a zero-copy binding to every light's flattened world matrix.
5. The animation loop maps the binding, runs the `animate_lights` Warp kernel to compute new transforms (orbit + vertical bob with phase offset per light), unmaps to flush writes to Fabric, then steps the renderer.

The robot scene is in centimeters, so the orbit radius (250 cm), bob amplitude (80 cm), and light intensity (100k) are tuned for that unit scale. If you adapt this to a meter-scale scene, divide the radius/amplitude by 100 and reduce intensity accordingly.

## Tweaking the effect

The motion lives entirely in the `animate_lights` Warp kernel in `main.py`. A few drop-in variations:

- **Vertical columns** — swap the `cos`/`sin` for `(orbit_radius, time*speed, 0)` to make the lights rise straight up.
- **Lissajous swarm** — use different frequencies for `x` and `z` (`cos(a*time)` vs `sin(b*time)`) to get figure-8s and rosette patterns.
- **Pulsing intensity** — bind a second attribute (`inputs:intensity`, scalar `float32`) and modulate it on a sine wave. Static colors plus pulsing intensity reads as flickering neon tubes.

## Saving a video

```bash
uv run main.py --png --no-rr
ffmpeg -framerate 60 -i _output/neon_robot_%03d.png -c:v libx264 -pix_fmt yuv420p neon_robot.mp4
```
