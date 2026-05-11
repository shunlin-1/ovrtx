#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["usd-core>=24.0", "numpy>=2.0"]
# ///
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary

"""Stand-alone USD pick-table collector — runs in its own ephemeral venv.

ovrtx 0.2.0 refuses to load when usd-core is *installed* in the same
environment because it bundles its own USD C++ libraries. This script
declares its own deps via PEP 723 inline metadata so `uv run` resolves
it into a separate ephemeral venv. The main agv venv stays clean of
usd-core, and we exchange data via a pickle file (numpy arrays don't
JSON-serialize cheaply).

For each Mesh prim we collect:
  * world-space AABB (broadphase reject)
  * world-space triangulated vertices (narrowphase ray-triangle test)
  * bound surface shader path + original emissive color/intensity
"""

import pickle
import sys

import numpy as np

from pxr import Usd, UsdGeom, UsdShade


def _world_triangles(prim, xform_cache) -> dict | None:
    """Return triangulated v0/v1/v2 arrays in world space, or None.

    Fan-triangulates polygons (assumed convex, which holds for the
    imagetostl meshes we ship). Skips meshes that are missing points
    or face data."""
    mesh = UsdGeom.Mesh(prim)
    points_attr  = mesh.GetPointsAttr()
    counts_attr  = mesh.GetFaceVertexCountsAttr()
    indices_attr = mesh.GetFaceVertexIndicesAttr()
    if not (points_attr.HasAuthoredValue()
            and counts_attr.HasAuthoredValue()
            and indices_attr.HasAuthoredValue()):
        return None

    points  = points_attr.Get()
    counts  = counts_attr.Get()
    indices = indices_attr.Get()
    if not points or not counts or not indices:
        return None

    # Fan triangulate. n=3 is one tri, n=4 is two tris, etc.
    tri_idx = []
    offset = 0
    for n in counts:
        for i in range(1, n - 1):
            tri_idx.append((indices[offset],
                            indices[offset + i],
                            indices[offset + i + 1]))
        offset += n
    if not tri_idx:
        return None
    tri_idx = np.asarray(tri_idx, dtype=np.int32)  # (T, 3)

    # Local-to-world transform. pxr's Gf.Matrix4d is row-major with the
    # row-vector convention (point * matrix), so np.array(xform) works
    # directly with right-multiplication: world = local_homog @ M.
    xform_4x4 = xform_cache.GetLocalToWorldTransform(prim)
    M = np.array(xform_4x4, dtype=np.float64)

    pts = np.asarray([(p[0], p[1], p[2]) for p in points], dtype=np.float64)
    pts_h = np.column_stack([pts, np.ones(len(pts), dtype=np.float64)])
    world = (pts_h @ M)[:, :3].astype(np.float32)

    return {
        "v0": world[tri_idx[:, 0]],
        "v1": world[tri_idx[:, 1]],
        "v2": world[tri_idx[:, 2]],
    }


def collect(sidecar_path: str) -> list[dict]:
    stage = Usd.Stage.Open(sidecar_path)
    if not stage:
        return []

    bbox_cache  = UsdGeom.BBoxCache(
        Usd.TimeCode.Default(),
        [UsdGeom.Tokens.default_],
        useExtentsHint=True)
    xform_cache = UsdGeom.XformCache(Usd.TimeCode.Default())

    out: list[dict] = []
    for prim in stage.Traverse():
        if not prim.IsA(UsdGeom.Mesh):
            continue

        rng = bbox_cache.ComputeWorldBound(prim).GetRange()
        if rng.IsEmpty():
            continue

        binding_api = UsdShade.MaterialBindingAPI(prim)
        material, _ = binding_api.ComputeBoundMaterial()
        if not material:
            continue

        src = material.ComputeSurfaceSource("mdl")
        if not src or not src[0]:
            src = material.ComputeSurfaceSource()
        if not src or not src[0]:
            continue

        shader = src[0]
        sp = shader.GetPrim()
        c_attr = sp.GetAttribute("inputs:emissive_color")
        i_attr = sp.GetAttribute("inputs:emissive_intensity")
        c0 = (c_attr.Get() if (c_attr and c_attr.HasValue())
              else (1.0, 1.0, 1.0))
        i0 = (i_attr.Get() if (i_attr and i_attr.HasValue()) else 0.0)

        tris = _world_triangles(prim, xform_cache)
        if tris is None:
            continue

        out.append({
            "mesh_path":      str(prim.GetPath()),
            "bb_min":         np.asarray(rng.GetMin(), dtype=np.float64),
            "bb_max":         np.asarray(rng.GetMax(), dtype=np.float64),
            "shader_path":    str(shader.GetPath()),
            "orig_color":     np.asarray([c0[0], c0[1], c0[2]],
                                          dtype=np.float32),
            "orig_intensity": np.float32(i0),
            **tris,
        })
    return out


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: pick_collector.py <sidecar.usda> <output.pkl>",
              file=sys.stderr)
        return 1
    data = collect(sys.argv[1])
    with open(sys.argv[2], "wb") as f:
        pickle.dump(data, f, protocol=pickle.HIGHEST_PROTOCOL)
    print(f"wrote {len(data)} pickable meshes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
