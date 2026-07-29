#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary

"""AGV viewer — ovrtx renders Test.usda, PySide6 + QML overlay on top.

Decoupled UI / render: ovrtx_step runs on a worker thread so drag and
scroll stay smooth at native Qt rate while the scene refreshes
asynchronously. (Same architecture as neon-robot-bim, AGV-tailored.)

The on-disk AGV USD has no camera or render product authored, so we
generate a sidecar `agv_render.usda` at startup that:
  * references the AGV scene file
  * adds /World/Camera with an orbit-controllable omni:xform
  * adds /Render/Camera with `omni:rtx:rendermode = "RaytracedLighting"`
ovrtx loads the sidecar instead, sees both the geometry (via reference)
and our render plumbing.

Usage:
    uv run main.py [--usd PATH]
"""

import argparse
import math
import pickle
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np

from PySide6.QtCore import (
    QObject, QSize, QUrl, Signal, Slot, Property, Qt
)
from PySide6.QtGui  import QGuiApplication, QImage
from PySide6.QtQml  import QQmlApplicationEngine
from PySide6.QtQuick import QQuickImageProvider

from ovrtx import Device, PrimMode, Renderer, RendererConfig

# Note: we deliberately do NOT `import pxr` in this process. ovrtx
# bundles its own USD libraries and refuses to load if the `pxr`
# Python module is already in sys.modules. The bbox walk that needs
# pxr lives in pick_collector.py, which we exec as a subprocess.

# Camera lens in the sidecar — used both there and at pick time so the
# screen-pixel → world-ray math stays in sync.
CAMERA_FOCAL_LENGTH       = 18.5
CAMERA_HORIZONTAL_APERTURE = 20.955
CAMERA_VERTICAL_APERTURE   = 11.787
RENDER_WIDTH  = 1920
RENDER_HEIGHT = 1080

NEON_EMISSIVE_COLOR     = np.array([0.30, 0.90, 1.00], dtype=np.float32)
NEON_EMISSIVE_INTENSITY = np.float32(10000.0)

# Xray modes lower OmniPBR `opacity_constant` so the surface goes
# semi-transparent. Two visually-distinct presets:
#   xray       — pure ghost: opacity 0.15, no emission
#   xray-light — translucent + cyan emission, "neon hologram" look
XRAY_OPACITY              = np.float32(0.15)
XRAY_LIGHT_OPACITY        = np.float32(0.40)
XRAY_LIGHT_EMISSIVE_COLOR = np.array([0.30, 0.90, 1.00], dtype=np.float32)
# Lower than NEON_EMISSIVE_INTENSITY so the glow doesn't blow out the
# semi-transparent surface (we still want the geometry to read).
XRAY_LIGHT_EMISSIVE_INTENSITY = np.float32(1500.0)

PICK_MODE_NEON       = "neon"
PICK_MODE_XRAY       = "xray"
PICK_MODE_XRAY_LIGHT = "xray-light"
PICK_MODES = (PICK_MODE_NEON, PICK_MODE_XRAY, PICK_MODE_XRAY_LIGHT)


SCRIPT_DIR  = Path(__file__).parent.resolve()
# Repo-relative default so the demo runs from a fresh clone on any
# machine without hand-editing paths. Override with --usd if you want
# to point at a USD elsewhere on disk.
DEFAULT_USD = SCRIPT_DIR / "Test.usda"
SIDECAR_USD = SCRIPT_DIR / "agv_render.usda"


# ════════════════════════════════════════════════════════════════════
# Source USD metadata sniffer
# ════════════════════════════════════════════════════════════════════
def parse_stage_metadata(usd_path: Path) -> dict:
    """Pull `upAxis` and `metersPerUnit` from a USD's top-level metadata
    block without dragging in a full pxr.Usd dependency. We only need
    them to size the camera distance + match scene units in the sidecar.

    Defaults match this project's working assumption: USDs come from
    Omniverse Composer-style workflows = Y-up, cm scale (mpu=0.01).
    Override via the file's own metadata if it specifies otherwise.
    """
    import re
    head = usd_path.read_text(encoding="utf-8", errors="ignore")[:4096]
    md = {"upAxis": "Y", "metersPerUnit": 0.01}
    m = re.search(r'upAxis\s*=\s*"([YZ])"', head)
    if m: md["upAxis"] = m.group(1)
    m = re.search(r'metersPerUnit\s*=\s*([\d.eE+-]+)', head)
    if m:
        try: md["metersPerUnit"] = float(m.group(1))
        except ValueError: pass
    return md


# ════════════════════════════════════════════════════════════════════
# Camera
# ════════════════════════════════════════════════════════════════════

class OrbitCamera:
    """Orbit / zoom camera. Configurable up-axis so it works on both
    Y-up scenes (Test.usda) and Z-up scenes (neon-only.usda)."""

    def __init__(self,
                 target=(0.0, 0.0, 0.0),
                 distance=4.0,
                 azimuth=math.radians(35),
                 elevation=math.radians(20),
                 up_axis: str = "Y"):
        self.target    = np.asarray(target, dtype=np.float64)
        self.distance  = distance
        self.azimuth   = azimuth
        self.elevation = elevation
        self.up_axis   = up_axis

    def orbit(self, dx_pixels: float, dy_pixels: float) -> None:
        sens = 0.005
        self.azimuth   -= dx_pixels * sens
        self.elevation += dy_pixels * sens
        self.elevation = max(math.radians(-89.0),
                             min(math.radians(89.0), self.elevation))

    def zoom(self, ticks: float) -> None:
        self.distance *= 0.9 ** ticks
        self.distance = max(0.1, min(500.0, self.distance))

    def matrix(self) -> np.ndarray:
        ce = math.cos(self.elevation); se = math.sin(self.elevation)
        ca = math.cos(self.azimuth);   sa = math.sin(self.azimuth)
        # Eye offset depends on which axis is "up". Standard spherical:
        #   Z-up:  offset = d*(ce*ca, ce*sa, se)
        #   Y-up:  offset = d*(ce*ca, se,    ce*sa)
        if self.up_axis.upper() == "Z":
            offset   = self.distance * np.array([ce * ca, ce * sa, se])
            world_up = np.array([0.0, 0.0, 1.0])
        else:
            offset   = self.distance * np.array([ce * ca, se, ce * sa])
            world_up = np.array([0.0, 1.0, 0.0])
        eye = self.target + offset

        forward = self.target - eye
        forward /= np.linalg.norm(forward)
        right = np.cross(forward, world_up)
        n = np.linalg.norm(right)
        right = right / n if n > 1e-6 else np.array([1.0, 0.0, 0.0])
        up = np.cross(right, forward)

        m = np.zeros((4, 4), dtype=np.float64)
        m[0, 0:3] = right
        m[1, 0:3] = up
        m[2, 0:3] = -forward     # camera looks down its local -Z
        m[3, 0:3] = eye
        m[3, 3]   = 1.0
        return m


# ════════════════════════════════════════════════════════════════════
# Sidecar USD (camera + render product) referencing the AGV scene
# ════════════════════════════════════════════════════════════════════

def write_sidecar(scene_usd: Path, out_path: Path,
                  rendermode: str = "RaytracedLighting",
                  up_axis: str = "Y",
                  meters_per_unit: float = 0.01) -> Path:
    """Write a USD that:
      * references the user's scene (`@<path>@`)
      * defines /World/Camera with omni:xform writable
      * defines /Render/Camera render product targeting that camera
      * matches the source's upAxis + metersPerUnit so our camera lives
        in the same coordinate space as the referenced geometry (a
        meters-units sidecar over a cm-units source would put the
        camera 100× too close).
      * sets RTX render mode (ovrtx 0.2.0 will warn + fall back to
        RealTimePathTracing if RaytracedLighting isn't supported.)
    """
    ref = scene_usd.as_posix()
    # Clipping range in *scene units*. Author values that cover ~1 mm
    # to ~100 km worth of meters regardless of mpu — safe for cm/mm/m
    # scenes alike, and Vulkan's 32-bit float depth tolerates it.
    near = 0.001 / meters_per_unit
    far  = 100000.0 / meters_per_unit
    out_path.write_text(f"""\
#usda 1.0
(
    defaultPrim = "World"
    upAxis = "{up_axis}"
    metersPerUnit = {meters_per_unit}
)

# Bring in the user's AGV scene as a reference under /Root.
def Xform "Root" (
    references = @{ref}@
)
{{
}}

# Our camera + transform handle. omni:xform is the matrix slot ovrtx
# expects when bind_attribute writes the orbit camera transform.
def Xform "World"
{{
    def Camera "Camera"
    {{
        token projection = "perspective"
        float focalLength = {CAMERA_FOCAL_LENGTH}
        float horizontalAperture = {CAMERA_HORIZONTAL_APERTURE}
        float verticalAperture = {CAMERA_VERTICAL_APERTURE}
        float2 clippingRange = ({near}, {far})
        custom matrix4d omni:xform = (
            (1, 0, 0, 0),
            (0, 1, 0, 0),
            (0, 0, 1, 0),
            (0, 0, 4, 1)
        )
        uniform token[] xformOpOrder = ["omni:xform"]
    }}
}}

def "Render"
{{
    def RenderProduct "Camera"
    {{
        rel camera = </World/Camera>
        rel orderedVars = </Render/Camera/LdrColor>
        int2 resolution = ({RENDER_WIDTH}, {RENDER_HEIGHT})
        custom token omni:rtx:rendermode = "{rendermode}"

        def RenderVar "LdrColor"
        {{
            string sourceName = "LdrColor"
        }}
    }}

    def RenderSettings "Settings"
    {{
        rel products = </Render/Camera>
        custom token omni:rtx:rendermode = "{rendermode}"
    }}
}}
""", encoding="utf-8")
    return out_path


# ════════════════════════════════════════════════════════════════════
# Pickable mesh table — built once at startup from the loaded stage
# ════════════════════════════════════════════════════════════════════

class PickInfo:
    """One pickable mesh: world AABB (broadphase reject) + actual
    triangulated world-space vertices (narrowphase ray-triangle test)
    + the OmniPBR shader prim path whose `inputs:emissive_color` /
    `inputs:emissive_intensity` we flip on click."""
    __slots__ = ("mesh_path", "bb_min", "bb_max",
                 "shader_path", "orig_color", "orig_intensity",
                 "v0", "v1", "v2")

    def __init__(self, mesh_path, bb_min, bb_max,
                 shader_path, orig_color, orig_intensity,
                 v0, v1, v2):
        self.mesh_path      = mesh_path
        self.bb_min         = bb_min
        self.bb_max         = bb_max
        self.shader_path    = shader_path
        self.orig_color     = orig_color
        self.orig_intensity = orig_intensity
        self.v0             = v0
        self.v1             = v1
        self.v2             = v2


def collect_mesh_picks(sidecar_path: Path) -> list[PickInfo]:
    """Run pick_collector.py via `uv run` (own ephemeral venv with
    usd-core). Receives a pickle file containing per-mesh world-space
    triangle data — JSON would balloon for a few hundred thousand
    floats, pickle keeps it as native numpy."""
    script  = SCRIPT_DIR / "pick_collector.py"
    pkl_out = sidecar_path.with_suffix(".picks.pkl")

    try:
        result = subprocess.run(
            ["uv", "run", str(script), str(sidecar_path), str(pkl_out)],
            capture_output=True, text=True, encoding="utf-8",
            check=False, timeout=300)
    except FileNotFoundError:
        print("[picks] `uv` not found on PATH — install uv or run "
              "pick_collector.py manually.")
        return []
    except Exception as e:
        print(f"[picks] subprocess launch failed: {e}")
        return []

    if result.returncode != 0:
        print(f"[picks] subprocess exited {result.returncode}")
        if result.stderr:
            print(result.stderr)
        return []

    try:
        with open(pkl_out, "rb") as f:
            data = pickle.load(f)
    except Exception as e:
        print(f"[picks] pickle load failed: {e}")
        return []

    picks = [PickInfo(**d) for d in data]
    n_tris = sum(len(p.v0) for p in picks)
    print(f"[picks] Indexed {len(picks)} pickable meshes "
          f"({n_tris} world-space triangles total)")
    return picks


def ray_triangles_min_t(origin, direction, V0, V1, V2):
    """Vectorized Möller-Trumbore. Returns the minimum t > 0 where the
    ray hits a triangle, or +inf if no hit. V0/V1/V2 each (N, 3)."""
    edge1 = V1 - V0
    edge2 = V2 - V0
    h = np.cross(direction[None, :], edge2)
    a = np.einsum("ij,ij->i", edge1, h)
    EPS = 1e-7
    valid = np.abs(a) > EPS
    if not valid.any():
        return float("inf")
    f = np.zeros_like(a)
    np.divide(1.0, a, out=f, where=valid)
    s = origin[None, :] - V0
    u = f * np.einsum("ij,ij->i", s, h)
    valid &= (u >= 0.0) & (u <= 1.0)
    q = np.cross(s, edge1)
    v = f * np.einsum("j,ij->i", direction, q)
    valid &= (v >= 0.0) & ((u + v) <= 1.0)
    t = f * np.einsum("ij,ij->i", edge2, q)
    valid &= t > EPS
    if not valid.any():
        return float("inf")
    return float(t[valid].min())


def ray_aabb(ray_origin: np.ndarray, ray_dir: np.ndarray,
             bb_min: np.ndarray, bb_max: np.ndarray):
    """Slab method ray-AABB intersection. Returns t > 0 along the ray
    if hit, else None. ray_dir does not need to be normalised — t is
    in units of |ray_dir|."""
    # Avoid divide-by-zero on axis-aligned rays via small epsilon.
    d = np.where(np.abs(ray_dir) < 1e-12,
                 np.copysign(1e-12, ray_dir), ray_dir)
    t1 = (bb_min - ray_origin) / d
    t2 = (bb_max - ray_origin) / d
    tmin = np.minimum(t1, t2).max()
    tmax = np.maximum(t1, t2).min()
    if tmax < max(tmin, 0.0):
        return None
    return tmin if tmin > 0.0 else tmax


# ════════════════════════════════════════════════════════════════════
# Frame provider: numpy → QImage for QML
# ════════════════════════════════════════════════════════════════════

class OvrtxFrameProvider(QQuickImageProvider):
    def __init__(self) -> None:
        super().__init__(QQuickImageProvider.ImageType.Image)
        self._image: QImage = QImage(1, 1, QImage.Format.Format_RGBA8888)
        self._image.fill(Qt.GlobalColor.transparent)

    def requestImage(self, id: str, size: QSize,
                     requested_size: QSize) -> QImage:
        return self._image

    def update(self, np_array: np.ndarray) -> None:
        if np_array.dtype != np.uint8:
            np_array = (np.clip(np_array, 0.0, 1.0) * 255.0).astype(np.uint8)
        arr = np.ascontiguousarray(np_array)
        h, w = arr.shape[:2]
        c = arr.shape[2] if arr.ndim == 3 else 1
        if c == 4:
            fmt = QImage.Format.Format_RGBA8888
        elif c == 3:
            fmt = QImage.Format.Format_RGB888
        else:
            return
        img = QImage(arr.tobytes(), w, h, w * c, fmt)
        if not img.isNull():
            self._image = img.copy()


# ════════════════════════════════════════════════════════════════════
# ovrtx render loop on a dedicated worker thread
# ════════════════════════════════════════════════════════════════════

class OvrtxBackend(QObject):
    frameChanged    = Signal(int)
    pickModeChanged = Signal()

    def __init__(self,
                 provider: OvrtxFrameProvider,
                 sidecar_path: Path,
                 up_axis: str = "Y",
                 distance: float = 5.0,
                 picks: list | None = None) -> None:
        super().__init__()
        self._provider = provider
        self._frame = 0

        self._camera = OrbitCamera(up_axis=up_axis, distance=distance)
        self._camera_dirty = True
        self._camera_lock = threading.Lock()

        # Pick infrastructure — UI thread calls .pick() to push (x,y)
        # onto _pending_picks; the render worker drains the queue each
        # iteration, ray-tests against PickInfo geometry, and applies
        # the active "pick mode" effect via ovrtx_write_attribute.
        self._picks: list[PickInfo] = picks or []
        self._pending_picks: list[tuple[float, float]] = []
        self._pick_lock = threading.Lock()
        # shader_path -> {'bindings': dict, 'active_mode': str|None, 'pi': PickInfo}
        self._selected: dict[str, dict] = {}
        # Active pick mode: pressing a mesh applies this effect; pressing
        # again restores. Switching mode + pressing same mesh swaps
        # effect (restore-then-apply). UI sets via setPickMode().
        self._pick_mode: str = PICK_MODE_XRAY

        self._sidecar_path = sidecar_path
        self._stop_event = threading.Event()
        self._thread = threading.Thread(
            target=self._run, name="ovrtx-worker", daemon=True)
        self._thread.start()

    @Property(int, notify=frameChanged)
    def frameCounter(self) -> int:
        return self._frame

    @Slot(float, float)
    def orbit(self, dx: float, dy: float) -> None:
        with self._camera_lock:
            self._camera.orbit(dx, dy)
            self._camera_dirty = True

    @Slot(float)
    def zoom(self, ticks: float) -> None:
        with self._camera_lock:
            self._camera.zoom(ticks)
            self._camera_dirty = True

    @Slot(float, float)
    def pick(self, x_frac: float, y_frac: float) -> None:
        """Click coordinates in 0..1 normalized window space.
        Queued for processing by the render worker on its next iteration."""
        with self._pick_lock:
            self._pending_picks.append((x_frac, y_frac))

    @Property(str, notify=pickModeChanged)
    def pickMode(self) -> str:
        return self._pick_mode

    @Slot(str)
    def setPickMode(self, mode: str) -> None:
        if mode in PICK_MODES and mode != self._pick_mode:
            self._pick_mode = mode
            self.pickModeChanged.emit()
            print(f"[pick] mode -> {mode}")

    def stop(self) -> None:
        self._stop_event.set()
        self._thread.join(timeout=2.0)

    def _push_camera(self, binding) -> None:
        m = self._camera.matrix()
        with binding.map(device=Device.CPU) as mapping:
            np.from_dlpack(mapping.tensor)[0] = m

    def _process_picks(self, renderer) -> None:
        """Drain pick queue, run ray-AABB pick, toggle emissive on hit."""
        with self._pick_lock:
            if not self._pending_picks:
                return
            picks = self._pending_picks[:]
            self._pending_picks.clear()

        if not self._picks:
            return

        # Snapshot camera basis under lock, then release for picking math.
        with self._camera_lock:
            m = self._camera.matrix().copy()
        right    = m[0, 0:3]
        up_vec   = m[1, 0:3]
        forward  = -m[2, 0:3]   # row 2 stores -forward
        eye      = m[3, 0:3]

        # Convert lens spec to half-FOVs (independent of metersPerUnit
        # since focalLength + aperture are in mm by USD convention).
        h_fov_2 = math.atan(CAMERA_HORIZONTAL_APERTURE
                            / (2.0 * CAMERA_FOCAL_LENGTH))
        v_fov_2 = math.atan(CAMERA_VERTICAL_APERTURE
                            / (2.0 * CAMERA_FOCAL_LENGTH))

        for x_frac, y_frac in picks:
            # Window-normalised → NDC → camera-space ray dir.
            ndc_x = 2.0 * x_frac - 1.0
            ndc_y = 1.0 - 2.0 * y_frac
            dx = ndc_x * math.tan(h_fov_2)
            dy = ndc_y * math.tan(v_fov_2)
            world_dir = right * dx + up_vec * dy + forward
            n = np.linalg.norm(world_dir)
            if n < 1e-9:
                continue
            world_dir = world_dir / n

            # Broadphase reject by AABB, then exact ray-triangle test
            # against the meshes whose bbox the ray actually clipped.
            best_t = float("inf")
            best   = None
            for pi in self._picks:
                t_box = ray_aabb(eye, world_dir, pi.bb_min, pi.bb_max)
                if t_box is None or t_box >= best_t:
                    continue
                t_tri = ray_triangles_min_t(
                    eye, world_dir, pi.v0, pi.v1, pi.v2)
                if t_tri < best_t:
                    best_t = t_tri
                    best   = pi

            if best is None or best_t == float("inf"):
                print(f"[pick] miss at ({x_frac:.3f}, {y_frac:.3f})")
                continue

            print(f"[pick] hit: {best.mesh_path} (shader={best.shader_path})")
            self._apply_pick(renderer, best)

    # Names of the shader inputs we may bind; bool ones are best-effort.
    _BIND_FLOAT_VEC3 = (("inputs:emissive_color",     "emissive_color",     (3,)),)
    _BIND_FLOAT_SCAL = (("inputs:emissive_intensity", "emissive_intensity", (1,)),
                        ("inputs:opacity_constant",   "opacity_constant",   (1,)))
    _BIND_BOOL_FLAGS = ("inputs:enable_emission", "inputs:enable_opacity")

    def _ensure_sel(self, renderer, pi: PickInfo) -> dict | None:
        """Lazy-create all the bindings we may want to write on a shader.
        Cached in self._selected so re-clicks reuse the same bindings."""
        sel = self._selected.get(pi.shader_path)
        if sel is not None:
            return sel
        bindings: dict = {}
        try:
            for usd_name, key, shape in self._BIND_FLOAT_VEC3:
                bindings[key] = renderer.bind_attribute(
                    prim_paths=[pi.shader_path], attribute_name=usd_name,
                    dtype="float32", shape=shape,
                    prim_mode=PrimMode.MUST_EXIST)
            for usd_name, key, shape in self._BIND_FLOAT_SCAL:
                bindings[key] = renderer.bind_attribute(
                    prim_paths=[pi.shader_path], attribute_name=usd_name,
                    dtype="float32", shape=shape,
                    prim_mode=PrimMode.MUST_EXIST)
        except Exception as e:
            print(f"[pick] bind_attribute (float) failed: {e}")
            return None
        # bool flags — dtype name varies; try the common ones.
        for usd_name in self._BIND_BOOL_FLAGS:
            key = usd_name.split(":")[1]
            bound = None
            for dtype_try in ("bool", "uint8", "int32"):
                try:
                    bound = renderer.bind_attribute(
                        prim_paths=[pi.shader_path],
                        attribute_name=usd_name,
                        dtype=dtype_try, shape=(1,),
                        prim_mode=PrimMode.MUST_EXIST)
                    break
                except Exception:
                    continue
            bindings[key] = bound  # may be None
        sel = {"bindings": bindings, "active_mode": None, "pi": pi}
        self._selected[pi.shader_path] = sel
        return sel

    @staticmethod
    def _write(sel: dict, key: str, value) -> None:
        b = sel["bindings"].get(key)
        if b is None:
            return
        with b.map(device=Device.CPU) as m:
            np.from_dlpack(m.tensor)[0] = value

    def _restore(self, sel: dict) -> None:
        pi = sel["pi"]
        self._write(sel, "emissive_color",     pi.orig_color)
        self._write(sel, "emissive_intensity", pi.orig_intensity)
        self._write(sel, "enable_emission",    0)
        self._write(sel, "opacity_constant",   np.float32(1.0))
        self._write(sel, "enable_opacity",     0)
        sel["active_mode"] = None

    def _apply_mode(self, sel: dict, mode: str) -> None:
        if mode == PICK_MODE_NEON:
            self._write(sel, "emissive_color",     NEON_EMISSIVE_COLOR)
            self._write(sel, "emissive_intensity", NEON_EMISSIVE_INTENSITY)
            self._write(sel, "enable_emission",    1)
        elif mode == PICK_MODE_XRAY:
            self._write(sel, "opacity_constant",   XRAY_OPACITY)
            self._write(sel, "enable_opacity",     1)
        elif mode == PICK_MODE_XRAY_LIGHT:
            # Combo: translucent body + soft cyan emission ⇒ "hologram".
            self._write(sel, "opacity_constant",   XRAY_LIGHT_OPACITY)
            self._write(sel, "enable_opacity",     1)
            self._write(sel, "emissive_color",     XRAY_LIGHT_EMISSIVE_COLOR)
            self._write(sel, "emissive_intensity", XRAY_LIGHT_EMISSIVE_INTENSITY)
            self._write(sel, "enable_emission",    1)
        sel["active_mode"] = mode

    def _apply_pick(self, renderer, pi: PickInfo) -> None:
        """Click semantics:
          * if the clicked shader is already in the current pick mode,
            click acts as a toggle-off (restore to original).
          * otherwise, restore-first then apply the current mode —
            covers both the fresh-click and the mode-swap cases.
        """
        sel = self._ensure_sel(renderer, pi)
        if sel is None:
            return
        target = self._pick_mode
        if sel["active_mode"] == target:
            self._restore(sel)
            print(f"[pick] -> off ({pi.shader_path})")
        else:
            if sel["active_mode"] is not None:
                self._restore(sel)
            self._apply_mode(sel, target)
            print(f"[pick] -> {target} ({pi.shader_path})")

    def _run(self) -> None:
        config = RendererConfig()
        renderer = Renderer(config)
        print(f"[ovrtx-worker] Loading {self._sidecar_path}")
        # The sidecar is the root layer (it references the AGV scene itself),
        # so this is open_usd rather than add_usd_reference.
        renderer.open_usd(str(self._sidecar_path))

        try:
            camera_binding = renderer.bind_attribute(
                prim_paths=["/World/Camera"],
                attribute_name="omni:xform",
                dtype="float64",
                shape=(4, 4),
                prim_mode=PrimMode.MUST_EXIST,
            )
            print("[ovrtx-worker] Camera binding ready")
        except Exception as e:
            print(f"[ovrtx-worker] Could not bind /World/Camera: {e}")
            camera_binding = None

        last = time.monotonic()
        while not self._stop_event.is_set():
            now = time.monotonic()
            delta = now - last
            last = now

            if camera_binding is not None:
                with self._camera_lock:
                    dirty = self._camera_dirty
                    self._camera_dirty = False
                if dirty:
                    self._push_camera(camera_binding)

            # Process any queued click picks before stepping so the
            # toggle takes effect on this frame's render.
            self._process_picks(renderer)

            products = renderer.step(
                render_products={"/Render/Camera"},
                delta_time=delta,
            )
            for product in products.values():
                for frame in product.frames:
                    if "LdrColor" not in frame.render_vars:
                        continue
                    with frame.render_vars["LdrColor"].map(
                            device=Device.CPU) as mapping:
                        # DLPack directly off the mapping — MappedRenderVar
                        # .tensor is deprecated for single-tensor render vars.
                        arr = np.from_dlpack(mapping)
                        self._provider.update(arr)
                    self._frame += 1
                    self.frameChanged.emit(self._frame)
                    break
                break


# ════════════════════════════════════════════════════════════════════
# Entry point
# ════════════════════════════════════════════════════════════════════

def main() -> None:
    parser = argparse.ArgumentParser(description="AGV viewer (ovrtx + PySide6).")
    parser.add_argument("--usd", type=Path, default=DEFAULT_USD,
                        help=f"AGV scene to render (default: {DEFAULT_USD})")
    parser.add_argument("--rendermode",
                        default="RaytracedLighting",
                        choices=["RaytracedLighting", "PathTracing",
                                 "Real-Time Path-Tracing", "Minimal"],
                        help="USD omni:rtx:rendermode token (default: "
                             "RaytracedLighting; ovrtx 0.2.0 falls back to "
                             "Real-Time Path-Tracing if unsupported)")
    parser.add_argument("--distance", type=float, default=None,
                        help="Initial camera distance in source-USD units. "
                             "Defaults to 5 meters worth (5 / metersPerUnit).")
    args = parser.parse_args()

    if not args.usd.exists():
        print(f"USD file not found: {args.usd}", file=sys.stderr)
        sys.exit(1)

    md = parse_stage_metadata(args.usd)
    distance = args.distance if args.distance is not None \
                             else 5.0 / md["metersPerUnit"]
    print(f"[main] Source USD: upAxis={md['upAxis']}, "
          f"metersPerUnit={md['metersPerUnit']}, "
          f"camera distance={distance:.3f} (units)")

    sidecar = write_sidecar(args.usd, SIDECAR_USD,
                            rendermode=args.rendermode,
                            up_axis=md["upAxis"],
                            meters_per_unit=md["metersPerUnit"])
    print(f"[main] Wrote sidecar with rendermode={args.rendermode}: {sidecar}")

    # Build the pick table from the composed sidecar stage. Done in the
    # main thread before the worker spins up so we don't open the same
    # stage twice in parallel.
    picks = collect_mesh_picks(sidecar)

    app = QGuiApplication(sys.argv)
    engine = QQmlApplicationEngine()

    provider = OvrtxFrameProvider()
    engine.addImageProvider("ovrtx", provider)

    backend = OvrtxBackend(provider, sidecar,
                           up_axis=md["upAxis"], distance=distance,
                           picks=picks)
    engine.rootContext().setContextProperty("ovrtxBackend", backend)

    qml_main = SCRIPT_DIR / "qml" / "Main.qml"
    engine.load(QUrl.fromLocalFile(str(qml_main)))

    if not engine.rootObjects():
        print("Failed to load QML; aborting.", file=sys.stderr)
        sys.exit(1)

    app.aboutToQuit.connect(backend.stop)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
