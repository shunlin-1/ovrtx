#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary

"""PySide6 + QML overlay UI on top of a live ovrtx-rendered scene.

Architecture (decoupled UI / render):
- ``OvrtxFrameProvider`` (QQuickImageProvider) holds the latest rendered
  frame as a QImage; QML's Image element pulls from it via the
  ``image://ovrtx/...`` URL scheme.
- ``OvrtxBackend`` (QObject) owns the ovrtx Renderer and runs its render
  loop on a *dedicated worker thread*. The UI thread therefore never
  blocks on ``renderer.step`` (which is ~50 ms at 1080p path tracing) —
  drag, scroll, and hover stay smooth at native Qt rate while the scene
  refreshes asynchronously at whatever rate ovrtx can sustain.
- Camera state is shared with a ``threading.Lock``; mouse callbacks on
  the UI thread set ``_camera_dirty`` and the worker picks it up next
  iteration.
- Each completed frame the worker calls ``provider.update(...)`` (a
  numpy → QImage copy) and emits ``frameChanged`` (auto-queued across
  threads); QML's Image rebinds via the counter-stamped URL.

Usage:
    uv run main.py [--usd PATH]
"""

import argparse
import math
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


class OrbitCamera:
    """Orbit / zoom camera in spherical coordinates around a target.

    Z-up convention (matches neon-only.usda):
      - azimuth 0 looks along +X
      - elevation 0 is horizontal (XY plane), +90° looks straight down

    `matrix()` returns a 4x4 row-major numpy array with the camera-to-world
    transform laid out so its memory matches GLM's column-major-stored
    column-vector matrix in C++ (the format ovrtx expects).
    """

    def __init__(self,
                 target=(0.0, 0.0, 0.0),
                 distance=8.0,
                 azimuth=math.radians(45),
                 elevation=math.radians(20)):
        self.target    = np.asarray(target, dtype=np.float64)
        self.distance  = distance
        self.azimuth   = azimuth
        self.elevation = elevation

    def orbit(self, dx_pixels: float, dy_pixels: float) -> None:
        sens = 0.005
        self.azimuth   -= dx_pixels * sens
        self.elevation += dy_pixels * sens
        self.elevation = max(math.radians(-89.0),
                             min(math.radians(89.0), self.elevation))

    def zoom(self, ticks: float) -> None:
        self.distance *= 0.9 ** ticks
        self.distance = max(0.5, min(200.0, self.distance))

    def matrix(self) -> np.ndarray:
        ce = math.cos(self.elevation); se = math.sin(self.elevation)
        ca = math.cos(self.azimuth);   sa = math.sin(self.azimuth)
        offset = self.distance * np.array([ce * ca, ce * sa, se])
        eye = self.target + offset

        forward = self.target - eye
        forward /= np.linalg.norm(forward)
        world_up = np.array([0.0, 0.0, 1.0])
        right = np.cross(forward, world_up)
        n = np.linalg.norm(right)
        right = right / n if n > 1e-6 else np.array([1.0, 0.0, 0.0])
        up = np.cross(right, forward)

        m = np.zeros((4, 4), dtype=np.float64)
        m[0, 0:3] = right
        m[1, 0:3] = up
        m[2, 0:3] = -forward
        m[3, 0:3] = eye
        m[3, 3]   = 1.0
        return m


SCRIPT_DIR = Path(__file__).parent.resolve()
DEFAULT_USD = (SCRIPT_DIR.parent.parent / "c" / "neon-robot-c"
                                        / "neon-only.usda")


class OvrtxFrameProvider(QQuickImageProvider):
    """Serves the most recent ovrtx-rendered frame to QML.

    ``update(np_array)`` swaps in a new QImage (with detached buffer),
    so the QML render thread can sample it freely while the worker
    re-maps the next tensor.
    """

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

        bytes_per_line = w * c
        img = QImage(arr.tobytes(), w, h, bytes_per_line, fmt)
        if img.isNull():
            return
        # Atomic Python attribute swap — safe across threads under GIL.
        self._image = img.copy()


class OvrtxBackend(QObject):
    """ovrtx render loop running on a dedicated worker thread.

    The UI thread never enters ``renderer.step``; mouse callbacks only
    update camera state under a lock. The worker reads the dirty flag
    each iteration, pushes a transform if needed, then steps + fetches +
    publishes the frame.
    """

    frameChanged = Signal(int)

    def __init__(self,
                 provider: OvrtxFrameProvider,
                 usd_path: Path) -> None:
        super().__init__()
        self._provider = provider
        self._frame = 0

        self._camera = OrbitCamera()
        self._camera_dirty = True
        self._camera_lock = threading.Lock()

        self._usd_path = usd_path
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

    def stop(self) -> None:
        self._stop_event.set()
        self._thread.join(timeout=2.0)

    def _push_camera(self, binding) -> None:
        m = self._camera.matrix()
        with binding.map(device=Device.CPU) as mapping:
            tensor = np.from_dlpack(mapping.tensor)
            tensor[0] = m

    def _run(self) -> None:
        # Initialize ovrtx on the worker thread so its CUDA context
        # (and any thread-local state) lives where the render loop runs.
        config = RendererConfig()
        renderer = Renderer(config)
        print(f"[ovrtx-worker] Loading {self._usd_path}")
        renderer.open_usd(str(self._usd_path))

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

            # Push camera transform if mouse moved since last iteration.
            if camera_binding is not None:
                with self._camera_lock:
                    dirty = self._camera_dirty
                    self._camera_dirty = False
                if dirty:
                    self._push_camera(camera_binding)

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
                    # Auto-queued across threads — UI thread receives
                    # this in its event loop and rebinds the QML Image.
                    self.frameChanged.emit(self._frame)
                    break
                break


def main() -> None:
    parser = argparse.ArgumentParser(
        description="PySide6 + ovrtx BIM viewer demo.")
    parser.add_argument("--usd", type=Path, default=DEFAULT_USD,
                        help=f"USD scene to render (default: {DEFAULT_USD})")
    args = parser.parse_args()

    if not args.usd.exists():
        print(f"USD file not found: {args.usd}", file=sys.stderr)
        sys.exit(1)

    app = QGuiApplication(sys.argv)
    engine = QQmlApplicationEngine()

    provider = OvrtxFrameProvider()
    engine.addImageProvider("ovrtx", provider)

    backend = OvrtxBackend(provider, args.usd)
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
