# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary

"""Joint-control test: drive a 2-link robot's transforms via ovrtx.

Companion scene: ``robot.usda`` (two cubes, link_1 + link_2).

Pipeline:
    sin/cos joint signal  ─►  forward_kinematics()  ─►  (2, 4, 4) matrices
                                      │
                                      ▼
                              binding.write(...)  ─►  renderer.step()
                                      │
                                      ▼
                       periodic stage dump for visual inspection

The kinematic chain (row-vector convention — translation lives in row 3):

    link_1 frame  =  Rz(θ₀)
    link_2 frame  =  Rz(θ₁) · T(L₁,0,0) · Rz(θ₀)

In row-vector form, p_world = p_local @ M, so the chain reads
right-to-left: link_2's local point is first rotated by its own joint
(Rz θ₁), then translated by the link offset L₁, then rotated by the
shoulder joint Rz θ₀. That's how link_2 "orbits" when θ₀ moves.

Run:
    uv run joint_control.py                    # default: robot.usda
    uv run joint_control.py --usd path/to.usda
"""

from __future__ import annotations

import argparse
import math
import time
from pathlib import Path

import numpy as np

from ovrtx import Device, PrimMode, Renderer, Semantic


def get_stage_debug_dump(renderer: Renderer) -> str:
    """Snapshot the current stage to USDA text (for visual verification)."""
    products = renderer.step(
        render_products={"ovrtx_debug_dump_stage"}, delta_time=0.0)
    frame = products["ovrtx_debug_dump_stage"].frames[0]
    with frame.render_vars["debug"].map(device=Device.CPU) as mapping:
        return mapping.tensor.to_bytes().decode("utf-8")


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_USD = SCRIPT_DIR / "robot.usda"


# ───────────────────────────────────────────────────────────────────────
# Robot description — must match the USD asset's prim layout.
# ───────────────────────────────────────────────────────────────────────

LINK_PATHS: list[str] = [
    "/World/Robot/link_1",
    "/World/Robot/link_2",
]

# Distance between joint 0 (link_1 origin) and joint 1 (link_2 origin),
# measured along link_1's local X axis. Matches the initial pose in
# robot.usda.
LINK_OFFSET = 1.5


# ───────────────────────────────────────────────────────────────────────
# Row-vector 4x4 matrix helpers.
#
# USD / ovrtx convention: a point p (row vector) is transformed as
#     p' = p @ M
# Translation lives in the LAST ROW (M[3, 0:3]), not the last column.
# This is the opposite of column-vector libs like Eigen / glm — if you
# copy-paste FK from a robotics textbook, transpose before using.
# ───────────────────────────────────────────────────────────────────────

def rot_z_row(angle_rad: float) -> np.ndarray:
    """4x4 rotation about Z (row-vector form)."""
    c, s = math.cos(angle_rad), math.sin(angle_rad)
    return np.array([
        [ c,  s, 0, 0],
        [-s,  c, 0, 0],
        [ 0,  0, 1, 0],
        [ 0,  0, 0, 1],
    ], dtype=np.float64)


def trans_row(tx: float, ty: float, tz: float) -> np.ndarray:
    """4x4 translation (row-vector form — translation in row 3)."""
    M = np.eye(4, dtype=np.float64)
    M[3, 0] = tx
    M[3, 1] = ty
    M[3, 2] = tz
    return M


# ───────────────────────────────────────────────────────────────────────
# Forward kinematics for the 2-link chain.
# ───────────────────────────────────────────────────────────────────────

def forward_kinematics(joint_angles: np.ndarray) -> np.ndarray:
    """Per-link world-space transforms for the 2-DOF chain.

    Parameters
    ----------
    joint_angles : np.ndarray, shape (2,), radians
        [shoulder_angle, elbow_angle]

    Returns
    -------
    np.ndarray, shape (2, 4, 4), float64
        Row-vector 4x4 transforms for [link_1, link_2] in world space.
    """
    theta_0, theta_1 = float(joint_angles[0]), float(joint_angles[1])

    T_link_1 = rot_z_row(theta_0)

    # Row-vector chain reads right-to-left as applied to a point:
    #   first rotate by elbow,
    #   then translate +X by link length,
    #   then rotate by shoulder.
    T_link_2 = rot_z_row(theta_1) @ trans_row(LINK_OFFSET, 0, 0) \
                                  @ rot_z_row(theta_0)

    return np.stack([T_link_1, T_link_2], axis=0)


# ───────────────────────────────────────────────────────────────────────
# Synthetic joint signal — replace with ROS / encoder feed in production.
# ───────────────────────────────────────────────────────────────────────

def demo_joint_signal(t: float) -> np.ndarray:
    """Lissajous-ish trajectory so both joints move at different rates."""
    return np.array([
        0.8 * math.sin(t * 0.6),        # shoulder: ±46°, slow
        1.2 * math.sin(t * 1.4 + 0.3),  # elbow:    ±69°, faster
    ], dtype=np.float64)


# ───────────────────────────────────────────────────────────────────────
# Driver loop.
# ───────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="2-link joint control test.")
    parser.add_argument("--usd", type=Path, default=DEFAULT_USD,
                        help=f"USD scene (default: {DEFAULT_USD.name})")
    parser.add_argument("--frames", type=int, default=120,
                        help="Frames to drive (default: 120)")
    parser.add_argument("--dump-every", type=int, default=30,
                        help="Write stage dump every N frames (0=off)")
    args = parser.parse_args()

    if not args.usd.exists():
        raise SystemExit(f"USD not found: {args.usd}")

    renderer = Renderer()
    renderer.add_usd(str(args.usd))

    # One-time binding — cached for every subsequent write.
    binding = renderer.bind_attribute(
        prim_paths=LINK_PATHS,
        attribute_name="omni:xform",
        semantic=Semantic.XFORM_MAT4x4,
        prim_mode=PrimMode.CREATE_NEW,
    )

    dump_dir = SCRIPT_DIR / "_joint_dumps"
    dump_dir.mkdir(exist_ok=True)

    t_start = time.time()
    for frame in range(args.frames):
        t = time.time() - t_start

        angles = demo_joint_signal(t)
        matrices = forward_kinematics(angles)

        binding.write(matrices)
        renderer.step(render_products={"ovrtx_debug_dump_stage"}, delta_time=0.0)

        if frame % 10 == 0:
            deg = np.degrees(angles)
            print(f"[frame {frame:3d}] θ₀={deg[0]:+6.1f}°  θ₁={deg[1]:+6.1f}°  "
                  f"link_2 pos=({matrices[1,3,0]:+.3f}, "
                  f"{matrices[1,3,1]:+.3f}, {matrices[1,3,2]:+.3f})")

        if args.dump_every and frame % args.dump_every == 0:
            dump = get_stage_debug_dump(renderer)
            (dump_dir / f"frame_{frame:04d}.usda").write_text(
                dump, encoding="utf-8")

    binding.unbind()

    print(f"\n[done] Stage dumps in: {dump_dir}")
    print("       Open any frame_*.usda in usdview / Omniverse / Blender")
    print("       to visually confirm the cubes moved to different poses.")


if __name__ == "__main__":
    main()
