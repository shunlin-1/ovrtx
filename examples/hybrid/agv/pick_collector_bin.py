#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["usd-core>=24.0", "numpy>=2.0"]
# ///
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary

"""Binary pick-table collector for the C++ AGV viewer.

Walks the loaded USD stage and emits a compact binary file the C++
side reads with plain fread. Runs in an ephemeral uv venv because
usd-core cannot coexist with ovrtx in one process.

v2 collects EVERY UsdGeomMesh with a non-empty world AABB. Meshes
that lack a bound MDL shader still appear (with shader_path and
material_name empty) so the C++ Y-clip path can hide them. The
X-ray-Neon path filters out entries with empty shader_path so it
only writes to materials it can actually modify.

Binary layout (little-endian, no padding):

    HEADER (16 B)
        u32 magic, version, mesh_count, reserved

    PER MESH:
        u32 mesh_path_len      bytes mesh_path
        u32 shader_path_len    bytes shader_path  (may be 0)
        u32 material_name_len  bytes material_name  (may be 0)
        f64 bb_min[3] / bb_max[3]
        f32 orig_color[3] / orig_intensity
"""

import struct
import sys

from pxr import Usd, UsdGeom, UsdShade


MAGIC = 0x50564741   # "AGVP"
VERSION = 2


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

        # Try to resolve a bound MDL shader — optional. Meshes without
        # one still go in the pick table so Y-clip can hide them; X-ray
        # ignores entries with an empty shader_path.
        shader_path = ""
        material_name = ""
        orig_color = (1.0, 1.0, 1.0)
        orig_intensity = 0.0

        binding_api = UsdShade.MaterialBindingAPI(prim)
        material, _ = binding_api.ComputeBoundMaterial()
        if material:
            src = material.ComputeSurfaceSource("mdl")
            if not src or not src[0]:
                src = material.ComputeSurfaceSource()
            if src and src[0]:
                shader = src[0]
                sp = shader.GetPrim()
                shader_path = str(shader.GetPath())
                material_name = material.GetPrim().GetName()
                c_attr = sp.GetAttribute("inputs:emissive_color")
                i_attr = sp.GetAttribute("inputs:emissive_intensity")
                c0 = (c_attr.Get() if (c_attr and c_attr.HasValue())
                      else (1.0, 1.0, 1.0))
                i0 = (i_attr.Get() if (i_attr and i_attr.HasValue()) else 0.0)
                orig_color = (float(c0[0]), float(c0[1]), float(c0[2]))
                orig_intensity = float(i0)

        out.append({
            "mesh_path":      str(prim.GetPath()),
            "shader_path":    shader_path,
            "material_name":  material_name,
            "bb_min":         (rng.GetMin()[0], rng.GetMin()[1], rng.GetMin()[2]),
            "bb_max":         (rng.GetMax()[0], rng.GetMax()[1], rng.GetMax()[2]),
            "orig_color":     orig_color,
            "orig_intensity": orig_intensity,
        })
    return out


def write_binary(records: list[dict], out_path: str) -> None:
    with open(out_path, "wb") as f:
        f.write(struct.pack("<IIII", MAGIC, VERSION, len(records), 0))
        for r in records:
            mp = r["mesh_path"].encode("utf-8")
            sp = r["shader_path"].encode("utf-8")
            mn = r["material_name"].encode("utf-8")
            f.write(struct.pack("<I", len(mp))); f.write(mp)
            f.write(struct.pack("<I", len(sp))); f.write(sp)
            f.write(struct.pack("<I", len(mn))); f.write(mn)
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
    n_with = sum(1 for r in records if r["shader_path"])
    n_without = len(records) - n_with
    print(f"wrote {len(records)} meshes to {sys.argv[2]} (v{VERSION}): "
          f"{n_with} with MDL shader (X-ray-targetable), "
          f"{n_without} bare-mesh (Y-clip-only)", file=sys.stderr)
    write_binary(records, sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
