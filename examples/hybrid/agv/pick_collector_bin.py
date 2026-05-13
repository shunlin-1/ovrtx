#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["usd-core>=24.0", "numpy>=2.0"]
# ///
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary

"""Binary pick-table collector for the C++ AGV viewer.

v3 — adds triangle data for ray-triangle narrowphase picking.

Walks the loaded USD stage, finds all Mesh prims, fan-triangulates
their faces, transforms vertices to world space, and emits:

    HEADER (16 B)
        u32 magic   = 0x50564741  ("AGVP" forwards on LE)
        u32 version = 3
        u32 mesh_count
        u32 reserved = 0

    PER MESH (variable):
        u32 mesh_path_len      bytes mesh_path
        u32 shader_path_len    bytes shader_path  (may be 0)
        u32 material_name_len  bytes material_name  (may be 0)
        f64 bb_min[3] / bb_max[3]                       (broadphase)
        f32 orig_color[3] / orig_intensity              (for restore)
        u32 tri_count                                   (narrowphase)
        f32 v0[tri_count*3]   first  vertex of each tri (world space)
        f32 v1[tri_count*3]   second vertex
        f32 v2[tri_count*3]   third  vertex

Meshes without a bound MDL surface still appear (shader_path empty)
so they participate in picking but not in material modes. Meshes
with zero usable triangles are skipped — picking them is pointless.
"""

import struct
import sys

import numpy as np

from pxr import Usd, UsdGeom, UsdShade


MAGIC = 0x50564741   # "AGVP"
VERSION = 3


def _world_triangles(prim, xform_cache):
    """Fan-triangulate the mesh's faces and transform to world space.

    Returns (v0, v1, v2) as float32 (T, 3) arrays, or None if the
    mesh has no usable faces.
    """
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

    # Fan-triangulate: face with n verts → (n-2) tris.
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
    tri_idx = np.asarray(tri_idx, dtype=np.int32)   # (T, 3)

    # Local → world transform. pxr's Gf.Matrix4d is row-major /
    # row-vector convention, so `pts_h @ M` gives world coords.
    xform_4x4 = xform_cache.GetLocalToWorldTransform(prim)
    M = np.array(xform_4x4, dtype=np.float64)

    pts = np.asarray([(p[0], p[1], p[2]) for p in points], dtype=np.float64)
    pts_h = np.column_stack([pts, np.ones(len(pts), dtype=np.float64)])
    world = (pts_h @ M)[:, :3].astype(np.float32)

    return (world[tri_idx[:, 0]],
            world[tri_idx[:, 1]],
            world[tri_idx[:, 2]])


def collect(sidecar_path):
    stage = Usd.Stage.Open(sidecar_path)
    if not stage:
        return []

    # XformCache is all we need now — bbox is recomputed from the
    # world-space triangle vertices below, ensuring it's in the same
    # coordinate space as the narrowphase will use. (The previous
    # `BBoxCache(useExtentsHint=True)` returned bboxes from stale
    # authored `extent` attributes that were 100× off on the building.)
    xform_cache = UsdGeom.XformCache(Usd.TimeCode.Default())

    out = []
    n_mesh_total = 0
    skipped_invisible = 0
    skipped_proxy_guide = 0
    skipped_empty_bbox = 0
    skipped_no_tris = 0
    for prim in stage.Traverse():
        if not prim.IsA(UsdGeom.Mesh):
            continue
        n_mesh_total += 1

        imageable = UsdGeom.Imageable(prim)

        # Filter out invisible meshes. Test.usda authors a hidden
        # ground plane (`visibility = "invisible"`) — without this
        # check, its huge triangles catch every ray and emission
        # writes land on a surface the user can't see.
        if imageable.ComputeVisibility() == UsdGeom.Tokens.invisible:
            skipped_invisible += 1
            continue

        # Only skip *explicit* proxy/guide (editor chrome). Anything
        # without an authored purpose, or with purpose=default/render,
        # is kept — Composer-style BIM exports often leave purpose
        # unauthored on every mesh, which would otherwise look like
        # "no purpose" to a strict filter.
        purpose = imageable.ComputePurpose()
        if purpose == UsdGeom.Tokens.proxy or purpose == UsdGeom.Tokens.guide:
            skipped_proxy_guide += 1
            continue

        # Triangulate FIRST (so we have ground-truth world-space verts),
        # then compute the bbox from those verts. This guarantees the
        # bbox and the triangle data are in the same coordinate space —
        # avoids the "stale authored extent vs real points" mismatch
        # we hit on the building (extent in m, points in mm after
        # composition → bbox 100× off, AABB broadphase rejects rays
        # the geometry actually intercepts).
        tris = _world_triangles(prim, xform_cache)
        if tris is None:
            skipped_no_tris += 1
            continue
        v0, v1, v2 = tris

        # World-space bbox = min/max across all triangle vertices.
        all_verts = np.concatenate([v0, v1, v2], axis=0)
        rng_min = all_verts.min(axis=0).astype(np.float64)
        rng_max = all_verts.max(axis=0).astype(np.float64)
        if (rng_max - rng_min).max() <= 0.0:
            skipped_empty_bbox += 1
            continue

        # Material binding is optional. Meshes without it still get
        # picked (Y-clip / future overlay slots), but can't have their
        # MDL inputs flipped.
        shader_path = ""
        material_name = ""
        orig_color = (1.0, 1.0, 1.0)
        orig_intensity = 0.0

        binding_api = UsdShade.MaterialBindingAPI(prim)
        material, _ = binding_api.ComputeBoundMaterial()

        # GeomSubset fallback. BIM-style USDs author per-face material
        # bindings on `def GeomSubset` children of the Mesh, not on the
        # Mesh itself. ComputeBoundMaterial() on the Mesh returns
        # nothing in that case. To still get *some* material to toggle
        # on click, fall back to the first child GeomSubset that has a
        # bound material.
        #
        # TODO (user choice): pick which GeomSubset to use.
        # Options to consider:
        #   (a) First child in declaration order — simplest, what's
        #       below right now. Whoever the BIM exporter listed first
        #       wins. For 桂蘭樓 this tends to be the "interior"
        #       material (concrete vs. glass).
        #   (b) Subset with the LARGEST face count — heuristically the
        #       "dominant" material (e.g. concrete is usually >> glass
        #       on a wall mesh). Better visual match for a click, but
        #       costs an extra iteration to count.
        #   (c) Skip — leave material empty if the mesh itself has no
        #       direct binding. Loses the toggle but stays accurate.
        if not material or not material.GetPrim().IsValid():
            for child in prim.GetChildren():
                if child.GetTypeName() == "GeomSubset":
                    child_binding = UsdShade.MaterialBindingAPI(child)
                    child_material, _ = child_binding.ComputeBoundMaterial()
                    if child_material and child_material.GetPrim().IsValid():
                        material = child_material
                        break   # ← Option (a): first subset wins

        if material and material.GetPrim().IsValid():
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
            "bb_min":         (float(rng_min[0]), float(rng_min[1]), float(rng_min[2])),
            "bb_max":         (float(rng_max[0]), float(rng_max[1]), float(rng_max[2])),
            "orig_color":     orig_color,
            "orig_intensity": orig_intensity,
            "v0":             v0,
            "v1":             v1,
            "v2":             v2,
        })
    print(
        f"[collect] mesh prims seen: {n_mesh_total}  "
        f"kept: {len(out)}  "
        f"skipped (invisible: {skipped_invisible}, "
        f"proxy/guide: {skipped_proxy_guide}, "
        f"empty bbox: {skipped_empty_bbox}, "
        f"no triangles: {skipped_no_tris})",
        file=sys.stderr)
    return out


def write_binary(records, out_path):
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
            v0, v1, v2 = r["v0"], r["v1"], r["v2"]
            tri_count = v0.shape[0]
            f.write(struct.pack("<I", tri_count))
            # tobytes() is little-endian on every platform we ship to
            # (x86_64 + aarch64 desktop Linux/Win); avoids per-triangle
            # struct.pack overhead for the 184-mesh × ~10k-tri building.
            f.write(np.ascontiguousarray(v0, dtype=np.float32).tobytes())
            f.write(np.ascontiguousarray(v1, dtype=np.float32).tobytes())
            f.write(np.ascontiguousarray(v2, dtype=np.float32).tobytes())


def main():
    if len(sys.argv) < 3:
        print("usage: pick_collector_bin.py <sidecar.usda> <output.bin>",
              file=sys.stderr)
        return 1
    records = collect(sys.argv[1])
    n_tris = sum(r["v0"].shape[0] for r in records)
    n_with = sum(1 for r in records if r["shader_path"])
    print(f"wrote {len(records)} meshes "
          f"({n_tris} triangles total, {n_with} with MDL shader) "
          f"to {sys.argv[2]} (v{VERSION})", file=sys.stderr)
    write_binary(records, sys.argv[2])
    return 0


# (Debug note: `collect()` also increments local counters
#  `skipped_invisible` and `skipped_nonrender`. If picks still fail to
#  land on the right mesh, instrument those counters or assert on
#  expected mesh count before/after the filter.)


if __name__ == "__main__":
    sys.exit(main())
