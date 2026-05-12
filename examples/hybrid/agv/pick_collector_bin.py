#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["usd-core>=24.0", "numpy>=2.0"]
# ///
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary

"""Binary pick-table collector for the C++ AGV viewer.

Mirrors examples/python/agv/pick_collector.py but emits a self-describing
binary blob the C++ side can read with plain fread. Same PEP 723 venv
isolation: usd-core cannot share a process with ovrtx, so this script
runs in its own ephemeral uv venv.

Binary layout (little-endian, no padding):

    HEADER (16 B)
        u32 magic   = 0x50564741  ("AGVP" forwards on LE)
        u32 version = 1
        u32 mesh_count
        u32 reserved = 0

    PER MESH (variable):
        u32 mesh_path_len
        bytes[mesh_path_len]  mesh_path  (UTF-8, no null)
        u32 shader_path_len
        bytes[shader_path_len] shader_path
        f64 bb_min[3]
        f64 bb_max[3]
        f32 orig_color[3]
        f32 orig_intensity

v1 is broadphase-only (no triangle data). The narrowphase ray-triangle
pass can be re-added by appending tri_count + v0/v1/v2 arrays per record
and bumping the version stamp.
"""

import struct
import sys

from pxr import Usd, UsdGeom, UsdShade


MAGIC = 0x50564741   # "AGVP"
VERSION = 1


def collect(sidecar_path: str) -> list[dict]:
    stage = Usd.Stage.Open(sidecar_path)
    if not stage:
        return []

    bbox_cache = UsdGeom.BBoxCache(
        Usd.TimeCode.Default(),
        [UsdGeom.Tokens.default_],
        useExtentsHint=True)

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

        out.append({
            "mesh_path":      str(prim.GetPath()),
            "shader_path":    str(shader.GetPath()),
            "bb_min":         (rng.GetMin()[0], rng.GetMin()[1], rng.GetMin()[2]),
            "bb_max":         (rng.GetMax()[0], rng.GetMax()[1], rng.GetMax()[2]),
            "orig_color":     (float(c0[0]), float(c0[1]), float(c0[2])),
            "orig_intensity": float(i0),
        })
    return out


def write_binary(records: list[dict], out_path: str) -> None:
    with open(out_path, "wb") as f:
        f.write(struct.pack("<IIII", MAGIC, VERSION, len(records), 0))
        for r in records:
            mp = r["mesh_path"].encode("utf-8")
            sp = r["shader_path"].encode("utf-8")
            f.write(struct.pack("<I", len(mp))); f.write(mp)
            f.write(struct.pack("<I", len(sp))); f.write(sp)
            f.write(struct.pack("<3d", *r["bb_min"]))
            f.write(struct.pack("<3d", *r["bb_max"]))
            f.write(struct.pack("<3f", *r["orig_color"]))
            f.write(struct.pack("<f", r["orig_intensity"]))


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: pick_collector_bin.py <sidecar.usda> <output.bin>",
              file=sys.stderr)
        return 1
    records = collect(sys.argv[1])
    write_binary(records, sys.argv[2])
    print(f"wrote {len(records)} pickable meshes to {sys.argv[2]}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
