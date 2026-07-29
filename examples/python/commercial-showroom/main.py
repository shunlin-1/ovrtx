# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary

"""Render the Commercial_NVD ArchVis asset pack with ovrtx.

The pack (`examples/assets/Commercial_NVD@10013/`) is a *library* of
individual furniture assets, not a renderable scene: every `.usd` is a
Z-up, centimetre-unit `/World` Xform with no camera, no lights and no
RenderProduct. To put it in front of ovrtx we generate a sidecar stage
that references the assets and adds the missing camera / lights /
RenderProduct, then render that.

Three modes:

    uv run main.py --list                     # enumerate the pack
    uv run main.py --asset Seating/Monarch.usd # one asset, auto-framed
    uv run main.py                             # composed showroom scene

Asset bounds come from `asset_bbox.py`, which runs in its own ephemeral
uv venv because ovrtx bundles its own USD libraries and refuses to load
alongside `usd-core`.
"""

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import ovrtx
from PIL import Image

HERE = Path(__file__).resolve().parent
DEFAULT_PACK = (
    HERE.parent.parent / "assets" / "Commercial_NVD@10013" / "Assets" / "ArchVis" / "Commercial"
)

# The pack is authored in centimetres, Z-up. The sidecar stage matches
# that exactly so referenced geometry, our camera and our floor all live
# in one coordinate space (a metres sidecar over a cm source would put
# the camera 100x too close).
UP_AXIS = "Z"
METERS_PER_UNIT = 0.01

# 36 x 20.25 mm aperture = 16:9, so the render aspect matches the film back.
FOCAL_LENGTH = 35.0
H_APERTURE = 36.0
V_APERTURE = 20.25


# ════════════════════════════════════════════════════════════════════
# Showroom layout: (asset relative path, translate cm, rotate-Z degrees)
# ════════════════════════════════════════════════════════════════════

SHOWROOM = [
    # 300 x 120 cm conference table at the origin.
    ("Conference/Gilbert.usd", (0, 0, 0), 0),
    # Six chairs, three per long side, facing the table.
    ("Seating/Monarch.usd", (-105, -95, 0), 0),
    ("Seating/Monarch.usd", (0, -95, 0), 0),
    ("Seating/Monarch.usd", (105, -95, 0), 0),
    ("Seating/Monarch.usd", (-105, 95, 0), 180),
    ("Seating/Monarch.usd", (0, 95, 0), 180),
    ("Seating/Monarch.usd", (105, 95, 0), 180),
    # Credenza against the back wall.
    ("Storage/Contemporary/Contemporary_Hutch.usd", (0, 370, 0), 180),
    # Side grouping, camera left.
    ("Tables/OakTableSmall.usd", (-380, -230, 0), 0),
    ("Seating/Zinnie.usd", (-380, -110, 0), 180),
    # Storage + reception desk, camera right.
    ("Storage/WhiteHome01.usd", (390, 100, 0), -90),
    ("Reception/L_Desk.usd", (360, -280, 0), 135),
]

FLOOR_HALF = 700.0  # cm
WALL_HEIGHT = 320.0
WALL_Y = 400.0
WALL_X = -560.0


# ════════════════════════════════════════════════════════════════════
# Asset inspection (separate venv — see asset_bbox.py)
# ════════════════════════════════════════════════════════════════════

def inspect_assets(paths: list[Path]) -> list[dict]:
    """Return per-asset metadata + world bbox via the PEP 723 sidecar."""
    proc = subprocess.run(
        ["uv", "run", str(HERE / "asset_bbox.py"), *[str(p) for p in paths]],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"asset_bbox.py failed:\n{proc.stderr}")
    return json.loads(proc.stdout)


def list_assets(pack: Path) -> list[Path]:
    return sorted(p for p in pack.rglob("*.usd") if ".thumbs" not in p.parts)


# ════════════════════════════════════════════════════════════════════
# Camera math
# ════════════════════════════════════════════════════════════════════

def look_at_matrix(eye: np.ndarray, target: np.ndarray, up: np.ndarray) -> np.ndarray:
    """Row-vector (USD `matrix4d`) camera-to-world transform.

    A USD camera looks down its own -Z with +Y up, so the third row holds
    -forward and the last row holds the eye position.
    """
    forward = target - eye
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, up)
    right /= np.linalg.norm(right)
    cam_up = np.cross(right, forward)

    m = np.zeros((4, 4), dtype=np.float64)
    m[0, 0:3] = right
    m[1, 0:3] = cam_up
    m[2, 0:3] = -forward
    m[3, 0:3] = eye
    m[3, 3] = 1.0
    return m


def frame_bbox(
    bbox_min: np.ndarray,
    bbox_max: np.ndarray,
    azimuth_deg: float,
    elevation_deg: float,
    margin: float,
) -> np.ndarray:
    """Camera transform that fits `bbox` in frame from an orbit position."""
    center = (bbox_min + bbox_max) * 0.5
    radius = float(np.linalg.norm(bbox_max - bbox_min)) * 0.5

    # Fit the tighter of the two half-angles so nothing is cropped.
    half_fov = min(
        math.atan(H_APERTURE * 0.5 / FOCAL_LENGTH),
        math.atan(V_APERTURE * 0.5 / FOCAL_LENGTH),
    )
    distance = radius / math.tan(half_fov) * margin

    az, el = math.radians(azimuth_deg), math.radians(elevation_deg)
    offset = np.array(
        [
            math.cos(el) * math.sin(az),
            -math.cos(el) * math.cos(az),
            math.sin(el),
        ]
    )
    eye = center + offset * distance
    # Aim slightly below centre — reads more naturally for furniture.
    return look_at_matrix(eye, center - np.array([0.0, 0.0, radius * 0.08]), np.array([0.0, 0.0, 1.0]))


def asset_ref(path: Path) -> str:
    """Quote a path as a USD asset reference.

    The pack directory is named `Commercial_NVD@10013`, and `@` terminates
    a plain `@...@` asset path. Sdf's triple-delimiter form `@@@...@@@`
    exists exactly for this case.
    """
    p = path.as_posix()
    return f"@@@{p}@@@" if "@" in p else f"@{p}@"


def matrix_to_usda(m: np.ndarray) -> str:
    # repr() of a numpy scalar is "np.float64(...)" under numpy 2.x, so
    # go through float() to emit plain literals.
    rows = ", ".join("(" + ", ".join(f"{float(v):.9g}" for v in row) + ")" for row in m)
    return f"( {rows} )"


def rotate_z_translate(degrees: float, translate: tuple[float, float, float]) -> np.ndarray:
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    m = np.eye(4, dtype=np.float64)
    m[0, 0:2] = (c, s)
    m[1, 0:2] = (-s, c)
    m[3, 0:3] = translate
    return m


# ════════════════════════════════════════════════════════════════════
# Sidecar stage generation
# ════════════════════════════════════════════════════════════════════

def room_shell() -> str:
    """Floor + two walls with UsdPreviewSurface materials.

    The pack ships bare props, so without a floor the assets float in an
    empty dome and there is nothing for indirect light to bounce off.
    """
    h = FLOOR_HALF
    return f"""
    def Mesh "Floor" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [({-h}, {-h}, 0), ({h}, {-h}, 0), ({h}, {h}, 0), ({-h}, {h}, 0)]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (
            interpolation = "faceVarying"
        )
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "faceVarying"
        )
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/Shell/Looks/FloorMat>
    }}

    def Mesh "WallBack" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [
            ({-h}, {WALL_Y}, 0), ({-h}, {WALL_Y}, {WALL_HEIGHT}),
            ({h}, {WALL_Y}, {WALL_HEIGHT}), ({h}, {WALL_Y}, 0)
        ]
        normal3f[] normals = [(0, -1, 0), (0, -1, 0), (0, -1, 0), (0, -1, 0)] (
            interpolation = "faceVarying"
        )
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/Shell/Looks/WallMat>
    }}

    def Mesh "WallLeft" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [
            ({WALL_X}, {WALL_Y}, 0), ({WALL_X}, {WALL_Y}, {WALL_HEIGHT}),
            ({WALL_X}, {-h}, {WALL_HEIGHT}), ({WALL_X}, {-h}, 0)
        ]
        normal3f[] normals = [(1, 0, 0), (1, 0, 0), (1, 0, 0), (1, 0, 0)] (
            interpolation = "faceVarying"
        )
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/Shell/Looks/WallMat>
    }}

    def Scope "Looks"
    {{
        def Material "FloorMat"
        {{
            token outputs:surface.connect = </World/Shell/Looks/FloorMat/Shader.outputs:surface>

            def Shader "Shader"
            {{
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.22, 0.2, 0.19)
                float inputs:roughness = 0.35
                float inputs:metallic = 0
                token outputs:surface
            }}
        }}

        def Material "WallMat"
        {{
            token outputs:surface.connect = </World/Shell/Looks/WallMat/Shader.outputs:surface>

            def Shader "Shader"
            {{
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.78, 0.76, 0.72)
                float inputs:roughness = 0.7
                float inputs:metallic = 0
                token outputs:surface
            }}
        }}
    }}
"""


def write_scene(
    out_path: Path,
    instances: list[tuple[Path, np.ndarray]],
    camera_matrix: np.ndarray,
    resolution: tuple[int, int],
    rendermode: str,
    with_shell: bool,
    dome_intensity: float,
    scene_scale: float,
) -> Path:
    """Author the sidecar stage that makes the pack renderable."""
    # Clipping range in scene units (cm): ~1 mm to ~1 km.
    near = 0.001 / METERS_PER_UNIT
    far = 1000.0 / METERS_PER_UNIT

    refs = []
    for i, (asset, xform) in enumerate(instances):
        # Reference (not payload) so the geometry is composed immediately;
        # the prim name must be unique because the pack reuses asset names.
        name = f"{asset.stem.replace('-', '_')}_{i}"
        refs.append(
            f"""
    def Xform "{name}" (
        prepend references = {asset_ref(asset)}
    )
    {{
        # Static placement uses the standard xformOp so the generated stage
        # also opens correctly in usdview / Omniverse. (`omni:xform` is the
        # ovrtx-specific slot for transforms written at runtime.)
        matrix4d xformOp:transform = {matrix_to_usda(xform)}
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }}
"""
        )

    shell = f'def Xform "Shell"\n    {{{room_shell()}    }}\n' if with_shell else ""
    width, height = resolution

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        f"""#usda 1.0
(
    defaultPrim = "World"
    upAxis = "{UP_AXIS}"
    metersPerUnit = {METERS_PER_UNIT}
)

def Xform "World"
{{
    {shell}
    def Xform "Assets"
    {{{"".join(refs)}    }}

    def Xform "Lights"
    {{
        # Untextured dome: neutral sky fill, no HDRI dependency.
        def DomeLight "Sky"
        {{
            float inputs:intensity = {dome_intensity}
            color3f inputs:color = (0.62, 0.72, 0.92)
        }}

        def DistantLight "Sun"
        {{
            float inputs:intensity = {dome_intensity * 6.0}
            float inputs:angle = 1.2
            color3f inputs:color = (1, 0.96, 0.9)
            custom matrix4d omni:xform = {matrix_to_usda(
                look_at_matrix(
                    np.array([-1.0, 1.4, 1.8]) * scene_scale,
                    np.zeros(3),
                    np.array([0.0, 0.0, 1.0]),
                )
            )}
            uniform token[] xformOpOrder = ["omni:xform"]
        }}
    }}

    def Camera "Camera" (
        prepend apiSchemas = ["OmniRtxCameraAutoExposureAPI_1", "OmniRtxCameraExposureAPI_1"]
    )
    {{
        token projection = "perspective"
        float focalLength = {FOCAL_LENGTH}
        float horizontalAperture = {H_APERTURE}
        float verticalAperture = {V_APERTURE}
        float2 clippingRange = ({near}, {far})
        custom matrix4d omni:xform = {matrix_to_usda(camera_matrix)}
        uniform token[] xformOpOrder = ["omni:xform"]
        float exposure:fStop = 5
        float exposure:responsivity = 1
        float exposure:time = 0.02
    }}
}}

def "Render"
{{
    def RenderProduct "Product" (
        prepend apiSchemas = ["OmniRtxSettingsCommonAdvancedAPI_1", "OmniRtxSettingsRtAdvancedAPI_1"]
    )
    {{
        rel camera = </World/Camera>
        rel orderedVars = </Render/Vars/LdrColor>
        uniform int2 resolution = ({width}, {height})
        custom token omni:rtx:rendermode = "{rendermode}"
        token omni:rtx:background:source:type = "domeLight"
        # Auto exposure keeps the untextured dome + distant light in range
        # without hand-tuning intensities per asset.
        bool omni:rtx:autoExposure:enabled = 1
    }}

    def "Vars"
    {{
        def RenderVar "LdrColor"
        {{
            uniform string sourceName = "LdrColor"
            uniform token sourceType = "raw"
        }}
    }}
}}
""",
        encoding="utf-8",
    )
    return out_path


# ════════════════════════════════════════════════════════════════════
# Render
# ════════════════════════════════════════════════════════════════════

def render(scene: Path, out_png: Path, warmup: int) -> None:
    print("Creating renderer (first run compiles + caches shaders)...", file=sys.stderr)
    renderer = ovrtx.Renderer()

    print(f"Loading {scene}...", file=sys.stderr)
    renderer.open_usd(scene.as_posix())

    # Real-Time Path-Tracing accumulates samples across steps, so the
    # first frames are noisy. Step repeatedly and keep only the last.
    print(f"Stepping {warmup} frames...", file=sys.stderr)
    products = None
    for _ in range(warmup):
        products = renderer.step(render_products={"/Render/Product"}, delta_time=1.0 / 60)

    out_png.parent.mkdir(parents=True, exist_ok=True)
    for _name, product in products.items():
        for frame in product.frames:
            with frame.render_vars["LdrColor"].map(device=ovrtx.Device.CPU) as var:
                # np.from_dlpack gives a view into the mapping, so copy before
                # the with-block unmaps it.
                pixels = np.from_dlpack(var).copy()
            Image.fromarray(pixels).convert("RGB").save(out_png)
            print(f"Wrote {out_png} ({pixels.shape[1]}x{pixels.shape[0]})", file=sys.stderr)
            return
    raise RuntimeError("renderer.step() produced no frames")


# ════════════════════════════════════════════════════════════════════
# CLI
# ════════════════════════════════════════════════════════════════════

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack", type=Path, default=DEFAULT_PACK, help="Commercial_NVD .../ArchVis/Commercial directory")
    ap.add_argument("--list", action="store_true", help="list assets in the pack and exit")
    ap.add_argument("--asset", help="render a single asset (path relative to --pack) instead of the showroom")
    ap.add_argument("--out", type=Path, default=HERE / "build" / "render.png")
    ap.add_argument("--scene-out", type=Path, default=None, help="where to write the generated sidecar stage")
    ap.add_argument("--res", default="1920x1080", help="WIDTHxHEIGHT")
    ap.add_argument("--mode", default="Real-Time Path-Tracing", help="omni:rtx:rendermode")
    ap.add_argument("--warmup", type=int, default=64, help="steps to accumulate before reading the frame")
    ap.add_argument("--azimuth", type=float, default=32.0)
    ap.add_argument("--elevation", type=float, default=14.0)
    ap.add_argument("--margin", type=float, default=1.12, help="framing slack; >1 pulls the camera back")
    ap.add_argument("--dome-intensity", type=float, default=350.0)
    ap.add_argument("--no-shell", action="store_true", help="omit the floor/walls")
    ap.add_argument("--dry-run", action="store_true", help="write the sidecar stage but do not render")
    args = ap.parse_args()

    pack = args.pack.resolve()
    if not pack.is_dir():
        print(f"error: pack not found: {pack}", file=sys.stderr)
        return 1

    assets = list_assets(pack)
    if args.list:
        for p in assets:
            print(p.relative_to(pack))
        print(f"\n{len(assets)} assets", file=sys.stderr)
        return 0

    width, height = (int(v) for v in args.res.lower().split("x"))

    if args.asset:
        asset = (pack / args.asset).resolve()
        if not asset.is_file():
            print(f"error: no such asset: {asset}", file=sys.stderr)
            return 1
        instances = [(asset, np.eye(4))]
        info = inspect_assets([asset])[0]
        if not info["ok"]:
            print(f"error: {info['error']}", file=sys.stderr)
            return 1
        bbox_min = np.array(info["bbox_min"])
        bbox_max = np.array(info["bbox_max"])
        print(
            f"{args.asset}: {info['up_axis']}-up, mpu={info['meters_per_unit']:.4g}, "
            f"bbox {bbox_min.round(1)} -> {bbox_max.round(1)}",
            file=sys.stderr,
        )
        default_out = HERE / "build" / f"{asset.stem}.png"
    else:
        missing = [rel for rel, _, _ in SHOWROOM if not (pack / rel).is_file()]
        if missing:
            print(f"error: showroom assets missing from pack: {missing}", file=sys.stderr)
            return 1
        instances = [
            ((pack / rel).resolve(), rotate_z_translate(rot, tr)) for rel, tr, rot in SHOWROOM
        ]
        # Frame the furniture, not the room shell.
        bbox_min = np.array([-480.0, -380.0, 0.0])
        bbox_max = np.array([480.0, 400.0, 140.0])
        default_out = HERE / "build" / "showroom.png"

    out_png = args.out if args.out != HERE / "build" / "render.png" else default_out
    scene_out = args.scene_out or out_png.with_suffix(".usda")

    scene_scale = float(np.linalg.norm(bbox_max - bbox_min))
    camera_matrix = frame_bbox(bbox_min, bbox_max, args.azimuth, args.elevation, args.margin)

    write_scene(
        scene_out,
        instances,
        camera_matrix,
        (width, height),
        args.mode,
        with_shell=not args.no_shell,
        dome_intensity=args.dome_intensity,
        scene_scale=scene_scale,
    )
    print(f"Wrote sidecar stage {scene_out}", file=sys.stderr)
    if args.dry_run:
        return 0

    render(scene_out, out_png, args.warmup)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
