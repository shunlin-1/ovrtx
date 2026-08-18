#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = [
#     "ovstream",
#     "ovrtx",
#     "warp-lang",
#     "numpy",
#     "pillow",
# ]
# ///
"""Stream *any* USD scene over WebRTC with ovrtx + ovstream.

The stock `ovstream/examples/python/ovrtx_stream` demo streams one
hard-coded scene that already ships a camera and a RenderProduct. Most
real scenes -- including the ones in this fork -- do not. This script
closes that gap:

  * takes any local USD file or remote URL (`--usd`)
  * probes local files out-of-process for up-axis, metersPerUnit, world
    bbox, cameras, RenderProducts and lights (`scene_probe.py`)
  * generates a sidecar stage with a framed camera, a RenderProduct and
    (only if the scene is unlit) a dome + key light, when the scene has
    no RenderProduct of its own
  * orbits correctly in both Y-up and Z-up, at whatever scene scale
  * hands every frame to ovstream as a zero-copy CUDA buffer

Rendering stays in ovrtx, transport stays in ovstream, and neither
library knows the other exists.

Usage:
    uv run main.py                                  # stock robot scene, WebRTC
    uv run main.py --usd ../agv/Test.usda
    uv run main.py --usd scene.usd --transport webrtc --transport rtsp:8554
    uv run main.py --usd scene.usd --res 2560x1440 --fps 30

Then open <ovstream checkout>/examples/webrtc_client/index.html and connect to
127.0.0.1:49100 (or pass --serve-client to have this script host it).

Client controls (WebRTC / native / SHM -- RTSP has no input channel):
    left-drag    orbit
    wheel        zoom
    any key      resume auto-orbit
"""

import argparse
import http.server
import json
import math
import subprocess
import sys
import threading
import time
from functools import partial
from pathlib import Path

import numpy as np

try:
    import ovrtx
except ImportError:
    sys.exit("This demo requires ovrtx: pip install ovrtx")

try:
    import warp as wp
except ImportError:
    sys.exit("This demo requires NVIDIA Warp: pip install warp-lang")

import ovstream
import ovstream_utils

# Must happen before any wp.from_dlpack / wp.zeros call, including the ones
# in the warm-up step -- Warp's DLPack path asserts on an uninitialised runtime.
wp.init()

HERE = Path(__file__).parent


def find_webrtc_client() -> Path | None:
    """Locate ovstream's bundled browser client.

    ovstream is a separate package with its own checkout, so its examples are
    not at a fixed path relative to this file. Check the layouts that actually
    occur -- a sibling clone of ovstream next to the ovrtx checkout, or one
    beside this repo -- and let OVSTREAM_CLIENT_DIR override for anything else.
    """
    import os

    env = os.environ.get("OVSTREAM_CLIENT_DIR")
    candidates = [Path(env)] if env else []
    repo_root = HERE.parent.parent.parent          # <repo>/examples/python/<this>
    candidates += [
        repo_root.parent / "ovstream" / "examples" / "webrtc_client",
        repo_root / "ovstream" / "examples" / "webrtc_client",
        Path.home() / "Desktop" / "ovlibs" / "ovstream" / "examples" / "webrtc_client",
    ]
    for c in candidates:
        if (c / "index.html").is_file():
            return c
    return None


WEBRTC_CLIENT_DIR = find_webrtc_client()

# Stock scene from the ovrtx Python minimal example -- the default so the
# demo runs with no arguments and nothing on disk.
DEFAULT_USD = ("https://omniverse-content-production.s3.us-west-2.amazonaws.com"
               "/Samples/Robot-OVRTX/robot-ovrtx.usda")
DEFAULT_RENDER_PRODUCT = "/Render/Camera"
DEFAULT_CAMERA = "/World/Camera"

# Sidecar camera lens. Matches the fork's agv / commercial-showroom
# viewers so framing is comparable across all three.
FOCAL_LENGTH = 18.5
H_APERTURE = 20.955
V_APERTURE = 11.787

SIDECAR_CAMERA = "/World/StreamCamera"
SIDECAR_PRODUCT = "/Render/StreamProduct"
SIDECAR_CAMERA_L = "/World/StreamCameraL"
SIDECAR_CAMERA_R = "/World/StreamCameraR"
SIDECAR_PRODUCT_L = "/Render/StreamProductL"
SIDECAR_PRODUCT_R = "/Render/StreamProductR"

# Human interpupillary distance. Only used to give the stereo preview a
# plausible baseline -- in a real XR session the runtime supplies the actual
# per-eye poses from xrLocateViews and this value is ignored.
DEFAULT_IPD_M = 0.063

# Orbit / input tuning.
ORBIT_PERIOD_S = 20.0            # one full turn
PITCH_LIMIT = math.radians(75.0)  # never go over the pole
MOUSE_YAW_GAIN = 0.005            # radians per pixel
MOUSE_PITCH_GAIN = 0.005
WHEEL_GAIN_FRACTION = 0.06        # per notch, as a fraction of framing radius

PROTOCOL_MAP = {
    "webrtc": ovstream.ServerType.WEBRTC,
    "rtsp": ovstream.ServerType.RTSP,
    "native": ovstream.ServerType.NATIVE,
    "shm": ovstream.ServerType.SHM,
    "cudashm": ovstream.ServerType.CUDASHM,
}
# Transports with a reverse channel. RTSP has none, so RTSP clients only
# ever see the auto-orbit. Enumerated rather than negated so a future
# backend without input is not silently opted in.
INPUT_CAPABLE = (ovstream.ServerType.WEBRTC, ovstream.ServerType.NATIVE,
                 ovstream.ServerType.SHM, ovstream.ServerType.CUDASHM)


# ---------------------------------------------------------------------------
# Scene probing
# ---------------------------------------------------------------------------

def probe_scene(usd_path: Path) -> dict:
    """Run scene_probe.py in a subprocess and return its JSON.

    Out-of-process because ovrtx bundles its own USD runtime and refuses
    to initialise when `pxr` has already been imported into the process.
    """
    proc = subprocess.run(
        ["uv", "run", str(HERE / "scene_probe.py"), str(usd_path)],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"scene_probe.py failed:\n{proc.stderr.strip()}")

    probe = json.loads(proc.stdout)
    # usd-core has no resolver for the remote S3 asset paths these scenes
    # reference, so anything inside an unresolved reference -- typically the
    # dome light -- is invisible to the probe even though ovrtx will load it
    # fine. Surface that, because it is what makes `--lights auto` guess wrong.
    probe["unresolved_refs"] = "Could not open asset" in proc.stderr
    return probe


def usable_products(probe: dict) -> list[str]:
    """RenderProducts we can actually render through.

    Scenes authored in Kit carry a viewport texture product
    (/Render/OmniverseKit/HydraTextures/...) that exists to drive Kit's own
    viewport widget. It is not a product this app can step -- treating it as
    one renders nothing -- so it does not count when deciding whether the
    scene needs a sidecar. The fork's agv scene is exactly this case.
    """
    return [p for p in probe["render_products"]
            if not p.startswith("/Render/OmniverseKit/")]


# ---------------------------------------------------------------------------
# Camera math -- up-axis agnostic
# ---------------------------------------------------------------------------

class OrbitFrame:
    """Basis + framing distance for orbiting an arbitrary scene.

    `up` is the stage up-axis; `axis_a` / `axis_b` span the ground plane,
    so the same spherical parameterisation works for Y-up and Z-up
    stages without branching in the hot path.
    """

    def __init__(self, up_axis: str, target: np.ndarray, radius: float):
        if up_axis.upper() == "Y":
            self.up = np.array([0.0, 1.0, 0.0])
        else:
            self.up = np.array([0.0, 0.0, 1.0])
        self.axis_a = np.array([1.0, 0.0, 0.0])
        self.axis_b = np.cross(self.up, self.axis_a)

        self.target = target
        self.radius = radius

        # Fit the tighter half-angle so nothing is cropped either way.
        half_fov = min(math.atan(H_APERTURE * 0.5 / FOCAL_LENGTH),
                       math.atan(V_APERTURE * 0.5 / FOCAL_LENGTH))
        self.distance = radius / math.tan(half_fov) * 1.25 if radius > 0 else 5.0

    def matrix(self, theta: float, phi: float, distance: float,
               eye_offset: float = 0.0) -> np.ndarray:
        """Camera-to-world as a row-major (4, 4) float64 matrix.

        USD's Gf.Matrix4d is row-major and cameras look down local -Z, so
        rows 0..2 are (right, up, -forward) and row 3 is the eye.
        """
        cos_phi = math.cos(phi)
        eye = self.target + distance * (
            cos_phi * math.cos(theta) * self.axis_a
            + cos_phi * math.sin(theta) * self.axis_b
            + math.sin(phi) * self.up
        )

        forward = self.target - eye
        forward /= np.linalg.norm(forward)
        right = np.cross(forward, self.up)
        norm = np.linalg.norm(right)
        if norm < 1e-9:          # looking straight up/down the pole
            right = np.array([1.0, 0.0, 0.0])
        else:
            right /= norm
        cam_up = np.cross(right, forward)

        # Stereo: slide the eye along the camera's own right vector. Both eyes
        # keep the same orientation and converge at infinity (parallel gaze),
        # which is what an XR runtime expects -- it supplies the real per-eye
        # poses and asymmetric frusta; this is only a stand-in for measuring
        # the cost of rendering two views.
        if eye_offset:
            eye = eye + right * eye_offset

        m = np.eye(4, dtype=np.float64)
        m[0, :3] = right
        m[1, :3] = cam_up
        m[2, :3] = -forward
        m[3, :3] = eye
        return m


# ---------------------------------------------------------------------------
# Camera state shared between the input callback and the render loop
# ---------------------------------------------------------------------------
# The callback fires on ovstream's callback thread while the loop reads on
# the main thread. Plain dict rather than locks: every field is a single
# float/bool, writes are independent, and a torn read costs at most one
# frame of camera lag.

cam_state = {
    "orbit_enabled": True,
    "yaw_offset": 0.0,
    "pitch": 0.0,
    "zoom_delta": 0.0,
    "last_mouse_x": None,
    "last_mouse_y": None,
    "left_button": False,
}


def reset_camera():
    cam_state.update(orbit_enabled=True, yaw_offset=0.0, pitch=0.0, zoom_delta=0.0)


def make_input_handler(zoom_step: float, min_distance: float, max_distance: float):
    def on_input(event):
        if event.type == ovstream.InputEventType.MOUSE:
            mouse = event.mouse
            if mouse.type == ovstream.MouseEventType.BUTTON:
                if mouse.data == ovstream.MouseButton.LEFT:
                    down = mouse.button_state == ovstream.KeyState.DOWN
                    cam_state["left_button"] = down
                    if down:
                        # Re-anchor on press so the first MOVE after a
                        # button-down doesn't emit a huge delta from
                        # wherever the cursor happened to be.
                        cam_state["last_mouse_x"] = mouse.x
                        cam_state["last_mouse_y"] = mouse.y
            elif mouse.type == ovstream.MouseEventType.MOVE:
                if cam_state["left_button"]:
                    if cam_state["last_mouse_x"] is not None:
                        dx = mouse.x - cam_state["last_mouse_x"]
                        dy = mouse.y - cam_state["last_mouse_y"]
                        # Turntable convention: drag up tilts the camera
                        # down toward the floor, so screen-y inverts.
                        cam_state["yaw_offset"] -= dx * MOUSE_YAW_GAIN
                        cam_state["pitch"] = max(
                            -PITCH_LIMIT,
                            min(PITCH_LIMIT,
                                cam_state["pitch"] + dy * MOUSE_PITCH_GAIN))
                        cam_state["orbit_enabled"] = False
                    cam_state["last_mouse_x"] = mouse.x
                    cam_state["last_mouse_y"] = mouse.y
            elif mouse.type == ovstream.MouseEventType.WHEEL:
                # scroll_y is in notches; positive = away from the user.
                cam_state["zoom_delta"] = max(
                    min_distance,
                    min(max_distance,
                        cam_state["zoom_delta"] - mouse.scroll_y * zoom_step))
                cam_state["orbit_enabled"] = False
        elif event.type == ovstream.InputEventType.KEYBOARD:
            if event.keyboard.key_state == ovstream.KeyState.DOWN:
                reset_camera()
    return on_input


# ---------------------------------------------------------------------------
# Sidecar stage generation
# ---------------------------------------------------------------------------

def asset_ref(path: Path) -> str:
    """Quote a path as a USD asset reference.

    `@` terminates a plain `@...@` asset path, and this fork has asset
    directories with `@` in the name (`Commercial_NVD@10013`). Sdf's
    triple-delimiter form exists for exactly that case.
    """
    p = path.as_posix()
    return f"@@@{p}@@@" if "@" in p else f"@{p}@"


def matrix_to_usda(m: np.ndarray) -> str:
    rows = ", ".join("(" + ", ".join(f"{float(v):.9g}" for v in row) + ")" for row in m)
    return f"( {rows} )"


def distant_light_matrix(up_axis: str) -> np.ndarray:
    """Rotation that aims a DistantLight down onto the scene.

    A UsdLux DistantLight emits along its own -Z, so the matrix has to put
    -Z on the desired direction. Getting this wrong does not fail loudly --
    it just lights the sky instead of the model, leaving a flat, shadowless
    frame that looks like "the materials are broken". The direction has to
    follow the stage up-axis, which is why this is computed rather than
    hardcoded.
    """
    if up_axis.upper() == "Y":
        world_up = np.array([0.0, 1.0, 0.0])
        direction = np.array([-0.4, -0.85, -0.35])
    else:
        world_up = np.array([0.0, 0.0, 1.0])
        direction = np.array([-0.4, -0.35, -0.85])
    forward = direction / np.linalg.norm(direction)

    right = np.cross(forward, world_up)
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)

    m = np.eye(4, dtype=np.float64)
    m[0, :3] = right
    m[1, :3] = up
    m[2, :3] = -forward
    return m


def camera_block(name: str, matrix: np.ndarray, near: float, far: float) -> str:
    return f"""
    def Camera "{name}" (
        prepend apiSchemas = ["OmniRtxCameraAutoExposureAPI_1", "OmniRtxCameraExposureAPI_1"]
    )
    {{
        token projection = "perspective"
        float focalLength = {FOCAL_LENGTH}
        float horizontalAperture = {H_APERTURE}
        float verticalAperture = {V_APERTURE}
        float2 clippingRange = ({near}, {far})
        matrix4d xformOp:transform = {matrix_to_usda(matrix)}
        uniform token[] xformOpOrder = ["xformOp:transform"]
        float exposure:fStop = 5
        float exposure:responsivity = 1
        float exposure:time = 0.02
    }}
"""


def product_block(name: str, camera_path: str, width: int, height: int,
                  rendermode: str) -> str:
    return f"""
    def RenderProduct "{name}" (
        prepend apiSchemas = ["OmniRtxSettingsCommonAdvancedAPI_1", "OmniRtxSettingsRtAdvancedAPI_1"]
    )
    {{
        rel camera = <{camera_path}>
        rel orderedVars = </Render/Vars/LdrColor>
        uniform int2 resolution = ({width}, {height})
        custom token omni:rtx:rendermode = "{rendermode}"
        token omni:rtx:background:source:type = "domeLight"
        bool omni:rtx:autoExposure:enabled = 1
    }}
"""


def write_sidecar(out_path: Path, source: Path, probe: dict,
                  camera_matrix: np.ndarray, resolution: tuple[int, int],
                  rendermode: str, add_lights: bool,
                  stereo: bool = False, dome_intensity: float = 350.0) -> Path:
    """Author a stage that references `source` and adds render plumbing.

    The camera transform is authored as a real `xformOp:transform`, not
    `omni:xform`: the latter is ovrtx's *runtime* write slot and USD does
    not recognise it in `xformOpOrder`, so authoring through it leaves the
    camera at identity (world origin, looking down -Z, rendering nothing
    but background). The runtime orbit then writes `omni:xform` on top.
    """
    mpu = probe["meters_per_unit"] or 0.01
    up_axis = probe["up_axis"]
    width, height = resolution

    # Clipping in scene units: 1 mm to 10 km, but never tighter than the
    # scene itself needs (a cm-unit building is ~10^4 units across).
    near = 0.001 / mpu
    far = max(10_000.0 / mpu, float(np.linalg.norm(
        np.array(probe["bbox_max"]) - np.array(probe["bbox_min"]))) * 10.0
        if probe["bbox_min"] else 0.0)

    # A dome for ambient sky fill plus a distant sun. Intensities follow the
    # fork's commercial-showroom viewer, which is tuned for centimetre-scale
    # stages -- brighter values wash the whole frame out even with auto
    # exposure on.
    lights = f"""
    def DomeLight "StreamDome"
    {{
        float inputs:intensity = {dome_intensity}
        color3f inputs:color = (0.62, 0.72, 0.92)
        float inputs:exposure = 0
        token inputs:texture:format = "latlong"
    }}

    def DistantLight "StreamKey"
    {{
        float inputs:intensity = {dome_intensity * 6.0}
        float inputs:angle = 1.2
        color3f inputs:color = (1, 0.96, 0.9)
        matrix4d xformOp:transform = {matrix_to_usda(distant_light_matrix(up_axis))}
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }}
""" if add_lights else ""

    if stereo:
        # Both eyes are authored with the same transform; the runtime orbit
        # writes the real per-eye matrices into omni:xform every frame.
        cameras = (camera_block("StreamCameraL", camera_matrix, near, far)
                   + camera_block("StreamCameraR", camera_matrix, near, far))
        products = (product_block("StreamProductL", SIDECAR_CAMERA_L,
                                  width, height, rendermode)
                    + product_block("StreamProductR", SIDECAR_CAMERA_R,
                                    width, height, rendermode))
    else:
        cameras = camera_block("StreamCamera", camera_matrix, near, far)
        products = product_block("StreamProduct", SIDECAR_CAMERA,
                                 width, height, rendermode)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(f"""#usda 1.0
(
    defaultPrim = "World"
    upAxis = "{up_axis}"
    metersPerUnit = {mpu}
)

# Generated by demo/ovrtx-stream/main.py -- do not hand-edit.
# Source scene: {source}

def Xform "World"
{{
    def Xform "Scene" (
        prepend references = {asset_ref(source)}
    )
    {{
    }}
{lights}{cameras}}}

def "Render"
{{{products}
    def "Vars"
    {{
        def RenderVar "LdrColor"
        {{
            uniform string sourceName = "LdrColor"
            uniform token sourceType = "raw"
        }}
    }}
}}
""")
    return out_path


# ---------------------------------------------------------------------------
# Frame conversion
# ---------------------------------------------------------------------------

@wp.kernel
def blit_bgra(src: wp.array3d(dtype=wp.uint8),
              dst: wp.array3d(dtype=wp.uint8),
              x_offset: int):
    """Copy ovrtx's RGBA8 output into the stream buffer as BGRA8.

    `x_offset` places the image horizontally, which is what makes stereo a
    one-kernel job: left eye at 0, right eye at width. Mono passes 0 and the
    kernel degenerates to the old in-place channel swap plus a copy.
    """
    x, y = wp.tid()
    dst[y, x + x_offset, 0] = src[y, x, 2]
    dst[y, x + x_offset, 1] = src[y, x, 1]
    dst[y, x + x_offset, 2] = src[y, x, 0]
    dst[y, x + x_offset, 3] = src[y, x, 3]


# ---------------------------------------------------------------------------
# Bundled browser client
# ---------------------------------------------------------------------------

def serve_client(port: int) -> None:
    """Serve ovstream's bundled WebRTC client on localhost.

    Convenience only -- the client also works straight off the filesystem,
    so nothing here is allowed to take the renderer down with it. Ports in
    the 8080 range are popular; if the requested one is busy, walk forward
    a few and otherwise just point at the file on disk.
    """
    if WEBRTC_CLIENT_DIR is None:
        print("[client] ovstream's webrtc_client not found. Clone "
              "github.com/NVIDIA-Omniverse/ovstream next to this repo, or set "
              "OVSTREAM_CLIENT_DIR to its examples/webrtc_client directory.",
              file=sys.stderr)
        return

    handler = partial(http.server.SimpleHTTPRequestHandler,
                      directory=str(WEBRTC_CLIENT_DIR))
    # Silence per-request logging; it interleaves with the FPS counter.
    handler.log_message = lambda *a, **k: None

    for candidate in range(port, port + 10):
        try:
            server = http.server.ThreadingHTTPServer(("127.0.0.1", candidate),
                                                     handler)
        except OSError:
            continue
        # Daemon thread so Ctrl+C in the render loop still exits promptly.
        threading.Thread(target=server.serve_forever, daemon=True).start()
        if candidate != port:
            print(f"[client] port {port} was busy, using {candidate}")
        print(f"[client] http://127.0.0.1:{candidate}/index.html")
        return

    print(f"[client] ports {port}-{port + 9} are all in use; open "
          f"{WEBRTC_CLIENT_DIR / 'index.html'} directly instead",
          file=sys.stderr)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_transport(spec: str):
    """Parse `protocol[:detail]` into (ServerType, detail, label)."""
    parts = spec.split(":", 1)
    protocol = parts[0].lower()
    if protocol not in PROTOCOL_MAP:
        sys.exit(f"Unknown transport '{protocol}'. "
                 f"Choose from: {', '.join(PROTOCOL_MAP)}")

    server_type = PROTOCOL_MAP[protocol]
    if server_type in (ovstream.ServerType.SHM, ovstream.ServerType.CUDASHM):
        name = parts[1] if len(parts) > 1 else ""
        return server_type, name, f"{protocol.upper()}:{name or '<auto>'}"

    port = int(parts[1]) if len(parts) > 1 else 0
    default_port = 8554 if protocol == "rtsp" else 49100
    return server_type, port, f"{protocol.upper()}:{port or default_port}"


def parse_args():
    p = argparse.ArgumentParser(
        description="Stream any USD scene over WebRTC with ovrtx + ovstream.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage:", 1)[1] if "Usage:" in __doc__ else None)
    p.add_argument("--usd", default=DEFAULT_USD,
                   help="USD file path or URL (default: stock Robot-OVRTX scene)")
    p.add_argument("--transport", action="append", metavar="PROTO[:DETAIL]",
                   help="webrtc[:port] | rtsp[:port] | native[:port] | "
                        "shm[:name] | cudashm[:name]. Repeatable. "
                        "Default: webrtc")
    p.add_argument("--res", default="1920x1080", metavar="WxH",
                   help="render/stream resolution (default: 1920x1080)")
    p.add_argument("--fps", type=int, default=60, help="target FPS (default: 60)")
    p.add_argument("--render-product", default=None,
                   help="RenderProduct prim path in the scene. Default: "
                        f"{DEFAULT_RENDER_PRODUCT} for remote/authored scenes, "
                        f"{SIDECAR_PRODUCT} when a sidecar is generated.")
    p.add_argument("--camera", default=None,
                   help="Camera prim path to drive. Default: "
                        f"{DEFAULT_CAMERA} / {SIDECAR_CAMERA}")
    p.add_argument("--sidecar", choices=["auto", "always", "never"], default="auto",
                   help="generate a camera + RenderProduct sidecar stage "
                        "(default: auto -- only when the scene has none)")
    # ovrtx accepts exactly these three. An unrecognised value is not an
    # error -- ovrtx silently falls back to the default -- so a typo shows up
    # as "the render mode made no difference" rather than as a failure.
    # Validate here so that cannot happen.
    p.add_argument("--rendermode", default="Real-Time Path-Tracing",
                   choices=["Real-Time Path-Tracing", "PathTracing", "Minimal"],
                   help='RenderProduct render mode. "Real-Time Path-Tracing" '
                        '(RT2, the default) is path-traced with real-time '
                        'denoising; "PathTracing" accumulates to reference '
                        'quality; "Minimal" is rasterised and cheap.')
    p.add_argument("--dome-intensity", type=float, default=350.0,
                   help="sidecar dome light intensity; the sun is 6x this. "
                        "350 suits centimetre-scale stages (default)")
    p.add_argument("--lights", choices=["auto", "always", "never"], default="auto",
                   help="add dome+key light to the sidecar "
                        "(default: auto -- only when the scene is unlit)")
    p.add_argument("--serve-client", nargs="?", type=int, const=8080, default=None,
                   metavar="PORT",
                   help="serve the bundled WebRTC client (default port 8080)")
    p.add_argument("--no-orbit", action="store_true",
                   help="hold the initial framing instead of auto-orbiting")
    p.add_argument("--stereo", action="store_true",
                   help="render two eyes and stream them side-by-side. "
                        "Needs a local scene (a stereo sidecar is generated). "
                        "The streamed frame is 2*width x height.")
    p.add_argument("--ipd", type=float, default=DEFAULT_IPD_M * 1000.0,
                   metavar="MM",
                   help=f"stereo eye separation in millimetres "
                        f"(default {DEFAULT_IPD_M * 1000.0:.0f})")
    p.add_argument("--png", metavar="PATH", default=None,
                   help="render warm-up frames, save the result as a PNG and "
                        "exit. Checks lighting/framing without involving the "
                        "streaming stack at all.")
    p.add_argument("--warmup", type=int, default=40, metavar="N",
                   help="frames to render before --png (default 40; RT2 needs "
                        "them for texture streaming and convergence)")
    p.add_argument("--benchmark", type=int, metavar="FRAMES", default=None,
                   help="render FRAMES frames with no streaming server and "
                        "report FPS, then exit. Measures pure render cost "
                        "(the XR frame-budget question) without encode.")
    return p.parse_args()


def run_benchmark(renderer, product_paths, write_camera, orbit, frames: int,
                  move_camera: bool = True):
    """Render `frames` frames as fast as possible and report the rate.

    Deliberately no ovstream server: this isolates ovrtx's render cost so a
    render mode / resolution can be judged against an XR frame budget without
    the encoder in the way. The camera keeps moving because a still camera
    lets RT2 accumulate samples and flatters the result -- in XR the head
    never stops, so a moving camera is the honest measurement.
    """
    import time

    # Warm-up frames are excluded: the first ones pay shader compilation,
    # texture streaming and pipeline setup that a steady-state XR loop does
    # not pay every frame.
    warmup = min(20, max(5, frames // 10))
    theta0, phi0 = math.radians(45.0), math.radians(20.0)
    timings = []

    if not move_camera:
        # Static camera: pay the attribute write once, outside the timed loop.
        # Comparing against the moving case isolates how much of a frame is
        # spent in the (0.3-style, deprecated) bind_attribute write path
        # versus in rendering.
        write_camera(theta0, phi0, orbit.distance)

    for i in range(frames + warmup):
        # ~1 degree of orbit per frame: constant invalidation of any
        # temporal accumulation, which is what head motion does.
        if move_camera:
            write_camera(theta0 + math.radians(i), phi0, orbit.distance)
        start = time.perf_counter()
        products = renderer.step(render_products=set(product_paths),
                                 delta_time=1.0 / 90.0)
        # step() can return before the GPU is done; mapping the output forces
        # the wait, so the number measures a completed frame.
        for _name, product in products.items():
            for frame in product.frames:
                with frame.render_vars["LdrColor"].map(
                        device=ovrtx.Device.CUDA) as mapping:
                    wp.from_dlpack(mapping)
        wp.synchronize_device("cuda:0")
        elapsed = time.perf_counter() - start
        if i >= warmup:
            timings.append(elapsed)
        if i == warmup - 1:
            print(f"  warm-up done ({warmup} frames), measuring {frames} ...",
                  file=sys.stderr)

    timings.sort()
    median = timings[len(timings) // 2]

    # Second pass, pipelined: issue every step without mapping the output or
    # synchronising, then sync once at the end. The first pass measures
    # *latency* (a completed frame, CPU and GPU serialised, which is what an
    # XR frame must do); this measures *throughput* with CPU/GPU overlap
    # allowed. A large gap means the per-frame sync is the cost, not the work.
    wp.synchronize_device("cuda:0")
    pipe_start = time.perf_counter()
    for i in range(frames):
        if move_camera:
            write_camera(theta0 + math.radians(i), phi0, orbit.distance)
        renderer.step(render_products=set(product_paths), delta_time=1.0 / 90.0)
    wp.synchronize_device("cuda:0")
    pipelined = (time.perf_counter() - pipe_start) / frames

    return {
        "median_ms": median * 1000.0,
        "median_fps": 1.0 / median,
        "best_ms": timings[0] * 1000.0,
        "worst_ms": timings[-1] * 1000.0,
        "pipelined_ms": pipelined * 1000.0,
        "pipelined_fps": 1.0 / pipelined,
    }


def main():
    args = parse_args()

    try:
        width, height = (int(v) for v in args.res.lower().split("x"))
    except ValueError:
        sys.exit(f"--res must look like 1920x1080, got {args.res!r}")

    specs = [parse_transport(t) for t in (args.transport or ["webrtc"])]

    is_remote = "://" in args.usd
    usd_path = None if is_remote else Path(args.usd).expanduser().resolve()
    if usd_path is not None and not usd_path.is_file():
        sys.exit(f"No such USD file: {usd_path}")

    # ---- Decide what to render ------------------------------------------
    probe = None
    scene_to_open = args.usd
    render_product = args.render_product or DEFAULT_RENDER_PRODUCT
    camera_prim = args.camera or DEFAULT_CAMERA
    orbit = OrbitFrame("Z", np.zeros(3), 0.0)   # replaced below when probed

    if is_remote:
        if args.sidecar == "always":
            sys.exit("--sidecar always needs a local file (the probe cannot "
                     "resolve remote assets); download the scene first.")
        if args.stereo:
            sys.exit("--stereo needs a local scene: it authors a second camera "
                     "and RenderProduct into a generated sidecar, and the probe "
                     "cannot resolve remote assets. Download the scene first.")
        print(f"Remote scene, skipping probe. Assuming RenderProduct "
              f"{render_product} and camera {camera_prim}.", file=sys.stderr)
        # Stock-scene framing: Z-up, robot torso at ~0.8, eye 4.4 away.
        orbit = OrbitFrame("Z", np.array([0.0, 0.0, 0.8]), 0.0)
        orbit.distance = 4.4
    else:
        print(f"Probing {usd_path.name} ...", file=sys.stderr)
        probe = probe_scene(usd_path)
        products = usable_products(probe)
        skipped = len(probe["render_products"]) - len(products)
        print(f"  up-axis {probe['up_axis']}, "
              f"metersPerUnit {probe['meters_per_unit']}, "
              f"{len(probe['cameras'])} camera(s), "
              f"{len(products)} usable RenderProduct(s)"
              f"{f' ({skipped} Kit viewport product ignored)' if skipped else ''}, "
              f"{len(probe['lights'])} light(s)", file=sys.stderr)
        if probe["unresolved_refs"]:
            print("  note: some references did not resolve in the probe "
                  "(remote assets); light detection may be incomplete",
                  file=sys.stderr)

        # Stereo always needs our own sidecar: no authored scene ships two
        # eye cameras with matching RenderProducts.
        make_sidecar = (args.sidecar == "always" or args.stereo
                        or (args.sidecar == "auto" and not products))

        if probe["bbox_min"] is None:
            center = np.zeros(3)
            radius = 1.0 / (probe["meters_per_unit"] or 0.01)
            print("  no boundable geometry; framing a unit sphere",
                  file=sys.stderr)
        else:
            bmin = np.array(probe["bbox_min"], dtype=np.float64)
            bmax = np.array(probe["bbox_max"], dtype=np.float64)
            center = (bmin + bmax) * 0.5
            radius = float(np.linalg.norm(bmax - bmin)) * 0.5
        orbit = OrbitFrame(probe["up_axis"], center, radius)

        if make_sidecar:
            add_lights = (args.lights == "always"
                          or (args.lights == "auto" and not probe["lights"]))
            suffix = "_stereo" if args.stereo else ""
            sidecar = write_sidecar(
                HERE / "_generated" / f"{usd_path.stem}{suffix}_stream.usda",
                usd_path, probe,
                orbit.matrix(math.radians(45.0), math.radians(20.0), orbit.distance),
                (width, height), args.rendermode, add_lights,
                stereo=args.stereo, dome_intensity=args.dome_intensity)
            scene_to_open = str(sidecar)
            render_product = args.render_product or SIDECAR_PRODUCT
            camera_prim = args.camera or SIDECAR_CAMERA
            print(f"  wrote sidecar {sidecar.relative_to(HERE)}"
                  f"{' (stereo)' if args.stereo else ''}"
                  f"{' (+ lights)' if add_lights else ''}", file=sys.stderr)
        else:
            if args.camera is None and probe["cameras"]:
                camera_prim = probe["cameras"][0]
            if args.render_product is None and probe["render_products"]:
                render_product = probe["render_products"][0]
            print(f"  using authored {render_product} / {camera_prim}",
                  file=sys.stderr)

    # One binding covers both eyes: bind_attribute takes a list of prims and
    # hands back one (N, 4, 4) tensor, so the per-frame write stays a single
    # map for mono and stereo alike.
    if args.stereo:
        camera_paths = [SIDECAR_CAMERA_L, SIDECAR_CAMERA_R]
        product_paths = [SIDECAR_PRODUCT_L, SIDECAR_PRODUCT_R]
        ipd_scene_units = (args.ipd / 1000.0) / (
            probe["meters_per_unit"] if probe and probe["meters_per_unit"] else 1.0)
        eye_offsets = [-ipd_scene_units * 0.5, ipd_scene_units * 0.5]
        print(f"Stereo: IPD {args.ipd:.1f} mm = {ipd_scene_units:.4g} scene units",
              file=sys.stderr)
    else:
        camera_paths = [camera_prim]
        product_paths = [render_product]
        eye_offsets = [0.0]

    # ---- ovrtx ------------------------------------------------------------
    print("Creating ovrtx renderer (first run compiles and caches shaders) ...",
          file=sys.stderr)
    renderer = ovrtx.Renderer()
    print(f"Loading {scene_to_open}", file=sys.stderr)
    renderer.open_usd(scene_to_open)

    # Bind the camera xform once and write it every frame. `omni:xform` is
    # ovrtx's runtime transform slot; mapped to CPU because a 64-byte
    # matrix gains nothing from staying on the GPU.
    camera_binding = renderer.bind_attribute(
        prim_paths=camera_paths,
        attribute_name="omni:xform",
        dtype="float64",
        shape=(4, 4),
        prim_mode=ovrtx.PrimMode.MUST_EXIST,
    )
    n_eyes = len(camera_paths)

    def write_camera(theta: float, phi: float, distance: float):
        with camera_binding.map(device=ovrtx.Device.CPU) as m:
            view = wp.from_dlpack(m.tensor).numpy().reshape(n_eyes, 4, 4)
            for i, offset in enumerate(eye_offsets):
                view[i] = orbit.matrix(theta, phi, distance, eye_offset=offset)

    target_frame_time = 1.0 / args.fps

    # Warm-up step: discover the resolution ovrtx actually produces so the
    # server's declared extents match. A mismatch makes every stream_video
    # reject the frame.
    write_camera(math.radians(45.0), math.radians(20.0), orbit.distance)
    products = renderer.step(render_products=set(product_paths),
                             delta_time=target_frame_time)
    if not products:
        sys.exit(f"ovrtx produced no output for render product(s) "
                 f"{product_paths}. Check the paths exist in the scene.")
    if len(products) != len(product_paths):
        sys.exit(f"asked for {len(product_paths)} render products, got "
                 f"{sorted(products)}; the stereo sidecar is malformed.")
    first_frame = next(iter(next(iter(products.values())).frames))
    with first_frame.render_vars["LdrColor"].map(device=ovrtx.Device.CUDA) as warmup:
        # Pass the mapping straight to DLPack. `.tensor` is deprecated in
        # ovrtx 0.4 for single-tensor render vars (the stock ovstream example
        # still uses it and warns).
        tensor = wp.from_dlpack(warmup)
        if tensor.ndim != 3 or tensor.shape[2] != 4 or tensor.dtype != wp.uint8:
            sys.exit(f"LdrColor has unexpected shape/dtype {tensor.shape} / "
                     f"{tensor.dtype}; expected (H, W, 4) uint8.")
        out_h, out_w = int(tensor.shape[0]), int(tensor.shape[1])
    if (out_w, out_h) != (width, height):
        print(f"Note: scene renders at {out_w}x{out_h}, not the requested "
              f"{width}x{height}; streaming at the scene's resolution.",
              file=sys.stderr)
        width, height = out_w, out_h
    print(f"Render output: {width}x{height} RGBA8", file=sys.stderr)

    if args.png:
        # Warm-up matters: the first frames render with low-resolution mips
        # while textures stream in, and RT2 needs samples to converge. A PNG
        # grabbed on frame 1 looks broken even when the scene is fine.
        print(f"Rendering {args.warmup} warm-up frames ...", file=sys.stderr)
        for i in range(args.warmup):
            write_camera(math.radians(45.0), math.radians(20.0), orbit.distance)
            products = renderer.step(render_products=set(product_paths),
                                     delta_time=target_frame_time)

        tiles = []
        for path in product_paths:
            for frame in products[path].frames:
                with frame.render_vars["LdrColor"].map(
                        device=ovrtx.Device.CPU) as mapping:
                    tiles.append(np.array(wp.from_dlpack(mapping).numpy(),
                                          copy=True))
        image = np.concatenate(tiles, axis=1) if len(tiles) > 1 else tiles[0]

        # Report what is actually in the buffer, so "it looks black" becomes a
        # number instead of an argument.
        rgb = image[:, :, :3]
        print(f"  pixels {image.shape[1]}x{image.shape[0]}  "
              f"mean RGB {rgb.mean():.1f}  min {rgb.min()}  max {rgb.max()}  "
              f"non-black {100.0 * (rgb.max(axis=2) > 8).mean():.1f}%")

        out = Path(args.png).expanduser()
        out.parent.mkdir(parents=True, exist_ok=True)
        try:
            from PIL import Image
            Image.fromarray(image[:, :, :3]).save(out)
        except ImportError:
            # No Pillow in the PEP 723 deps: write a PPM, which every viewer
            # and ImageMagick reads, rather than failing the diagnostic.
            out = out.with_suffix(".ppm")
            with open(out, "wb") as f:
                f.write(f"P6\n{image.shape[1]} {image.shape[0]}\n255\n".encode())
                f.write(image[:, :, :3].tobytes())
        print(f"  wrote {out}")
        camera_binding.unbind()
        return

    if args.benchmark:
        px = width * height
        print(f"\nBenchmark: {width}x{height} ({px / 1e6:.2f} Mpx), "
              f"rendermode {args.rendermode!r}", file=sys.stderr)
        if args.no_orbit:
            print("  static camera: per-frame attribute write excluded",
                  file=sys.stderr)
        stats = run_benchmark(renderer, product_paths, write_camera, orbit,
                              args.benchmark, move_camera=not args.no_orbit)
        print(f"\n  median   {stats['median_ms']:7.2f} ms  "
              f"= {stats['median_fps']:6.2f} FPS   (latency: synced per frame)")
        print(f"  best     {stats['best_ms']:7.2f} ms")
        print(f"  worst    {stats['worst_ms']:7.2f} ms")
        print(f"  pipelined{stats['pipelined_ms']:7.2f} ms  "
              f"= {stats['pipelined_fps']:6.2f} FPS   (throughput: no per-frame sync)")
        print(f"  throughput {px * stats['median_fps'] / 1e6:7.1f} Mpx/s")
        # What this resolution would cost for two eyes at VR frame rates.
        for target_hz in (72, 90):
            need = px * 2 * target_hz / 1e6
            have = px * stats["median_fps"] / 1e6
            print(f"  stereo @ {target_hz} Hz needs {need:7.1f} Mpx/s "
                  f"-> {need / have:5.1f}x short")
        camera_binding.unbind()
        return

    # ---- ovstream ---------------------------------------------------------
    # One long-lived buffer: ovstream stages each frame into server-owned
    # memory before stream_video returns, so it is safe to overwrite on the
    # next iteration; reusing it just saves a per-frame allocation.
    # Stereo streams side-by-side in one frame: left eye occupies x [0, width),
    # right eye [width, 2*width). One video stream, one encode, and any SBS
    # viewer (or a phone in a cardboard holder) can display it.
    stream_width = width * n_eyes
    stream_buf = wp.zeros((height, stream_width, 4), dtype=wp.uint8,
                          device="cuda:0")
    draw_stream = wp.get_stream("cuda:0")
    draw_event = wp.Event(device="cuda:0")

    # ovrtx renders in its own CUDA context. StreamSDK needs that exact
    # context to read the frames: a device ordinal alone fails the encode
    # with "CUDA error invalid argument".
    cuda_context = int(wp.get_device("cuda:0").context)

    zoom_step = max(orbit.distance * WHEEL_GAIN_FRACTION, 1e-6)
    min_distance = -orbit.distance * 0.9    # deltas, added to orbit.distance
    max_distance = orbit.distance * 9.0

    if args.serve_client is not None:
        serve_client(args.serve_client)

    ovstream.initialize()
    servers = []
    frame_count = 0
    try:
        for server_type, detail, label in specs:
            s = ovstream.Server(server_type)
            s.on_connection = lambda c, lbl=label: print(
                f"\n[{lbl}] client {'connected' if c else 'disconnected'}")
            if server_type in INPUT_CAPABLE:
                s.on_input = make_input_handler(zoom_step, min_distance, max_distance)

            cfg = ovstream.ServerConfig(width=stream_width, height=height,
                                        cuda_device=0, cuda_context=cuda_context)
            if server_type == ovstream.ServerType.RTSP:
                if detail:
                    cfg.stream_port = detail
            elif server_type == ovstream.ServerType.SHM:
                if detail:
                    cfg.shm_stream_name = detail
            elif server_type == ovstream.ServerType.CUDASHM:
                if detail:
                    cfg.cudashm_stream_name = detail
            else:
                if detail:
                    cfg.webrtc_signal_port = detail
            s.start(cfg)
            servers.append(s)

            if server_type == ovstream.ServerType.RTSP:
                print(f"[{label}] ffplay rtsp://localhost:{detail or 8554}/stream")
            elif server_type in (ovstream.ServerType.SHM, ovstream.ServerType.CUDASHM):
                viewer = ("main_cudashm_viewer.py"
                          if server_type == ovstream.ServerType.CUDASHM
                          else "main_viewer.py")
                print(f"[{label}] attach: uv run "
                      f"<ovstream>/examples/python/local_stream/{viewer} "
                      f"{detail or '<see log for auto name>'}")
            else:
                print(f"[{label}] connect a client to 127.0.0.1:{detail or 49100}")
                if args.serve_client is None and WEBRTC_CLIENT_DIR:
                    print(f"          client: {WEBRTC_CLIENT_DIR / 'index.html'}")

        print("left-drag orbit · wheel zoom · any key resumes auto-orbit · "
              "Ctrl+C to stop")

        loop_start_ns = None
        with ovstream_utils.Loop(ovstream_utils.LoopConfig(fps_target=args.fps)) as loop:
            while True:
                tick = loop.tick()
                if loop_start_ns is None:
                    loop_start_ns = tick.start_time_ns
                elapsed = (tick.start_time_ns - loop_start_ns) / 1e9

                if cam_state["orbit_enabled"] and not args.no_orbit:
                    theta = math.radians(45.0) + elapsed * (2 * math.pi / ORBIT_PERIOD_S)
                    phi = math.radians(20.0)
                    distance = orbit.distance
                else:
                    theta = math.radians(45.0) + cam_state["yaw_offset"]
                    phi = cam_state["pitch"]
                    distance = max(orbit.distance * 0.05,
                                   orbit.distance + cam_state["zoom_delta"])
                write_camera(theta, phi, distance)

                # First tick reports delta_time_seconds == 0.0 (nothing to
                # measure against yet); fall back to the nominal target so
                # the first frame doesn't render with dt=0.
                delta_time = tick.delta_time_seconds or target_frame_time
                products = renderer.step(render_products=set(product_paths),
                                         delta_time=delta_time)

                # Blit each eye into its half of the side-by-side buffer.
                # Indexed by path rather than iterating the dict, so left and
                # right can never swap between frames.
                for eye, path in enumerate(product_paths):
                    for frame in products[path].frames:
                        with frame.render_vars["LdrColor"].map(
                                device=ovrtx.Device.CUDA) as mapping:
                            wp.launch(blit_bgra, dim=(width, height),
                                      inputs=[wp.from_dlpack(mapping),
                                              stream_buf, eye * width],
                                      device="cuda:0")
                draw_stream.record_event(draw_event)

                # The sync hint lets ovstream chain off Warp's CUDA stream
                # instead of forcing a host-side synchronize every frame.
                video_frame = ovstream.VideoFrame.from_cuda_array(
                    stream_buf,
                    sync=ovstream.CudaSync(stream=draw_stream.cuda_stream,
                                           wait_event=draw_event.cuda_event),
                )
                for s in servers:
                    try:
                        s.stream_video(video_frame)
                    except ovstream.OvstreamError:
                        pass    # no client connected yet; drop silently

                frame_count = tick.frame_index + 1
                print(f"\rFPS: {tick.stats.fps_current}   ", end="", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        for s in servers:
            try:
                s.stop()
            except ovstream.OvstreamError:
                pass
            s.close()
        camera_binding.unbind()
        ovstream.shutdown()
        print(f"\nDone. Streamed {frame_count} frames.")


if __name__ == "__main__":
    main()
