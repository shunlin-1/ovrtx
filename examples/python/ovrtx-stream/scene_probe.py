#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["usd-core"]
# ///
"""Probe a local USD file and print stage facts as JSON.

Runs as a *subprocess* on purpose. ovrtx bundles its own USD runtime and
refuses to initialise if `pxr` is already in `sys.modules`, so anything
that needs pxr (bbox walks, up-axis lookups) has to live outside the
renderer process. Same split the fork's `commercial-showroom/asset_bbox.py`
and `agv/pick_collector.py` use.

Output (stdout, one JSON object):

    {
      "up_axis": "Y" | "Z",
      "meters_per_unit": 0.01,
      "bbox_min": [x, y, z],
      "bbox_max": [x, y, z],
      "cameras": ["/World/Camera", ...],
      "render_products": ["/Render/Product", ...],
      "lights": ["/World/DomeLight", ...]
    }

`bbox_min` / `bbox_max` are null when the stage has no boundable geometry.
"""

import json
import sys

from pxr import Usd, UsdGeom


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: scene_probe.py <file.usd[a|c]>", file=sys.stderr)
        return 2

    stage = Usd.Stage.Open(sys.argv[1])
    if stage is None:
        print(f"could not open stage: {sys.argv[1]}", file=sys.stderr)
        return 1

    cameras, products, lights = [], [], []
    for prim in stage.Traverse():
        type_name = str(prim.GetTypeName())
        if type_name == "Camera":
            cameras.append(prim.GetPath().pathString)
        elif type_name == "RenderProduct":
            products.append(prim.GetPath().pathString)
        elif type_name.endswith("Light"):
            # DomeLight / DistantLight / SphereLight / RectLight / ...
            lights.append(prim.GetPath().pathString)

    # Default + render purposes: enough to bound visible geometry without
    # dragging in proxy/guide gizmos that would inflate the box.
    cache = UsdGeom.BBoxCache(
        Usd.TimeCode.Default(),
        [UsdGeom.Tokens.default_, UsdGeom.Tokens.render],
        useExtentsHint=True,
    )
    box = cache.ComputeWorldBound(stage.GetPseudoRoot()).ComputeAlignedRange()

    print(json.dumps({
        "up_axis": UsdGeom.GetStageUpAxis(stage),
        "meters_per_unit": UsdGeom.GetStageMetersPerUnit(stage),
        "bbox_min": None if box.IsEmpty() else list(box.GetMin()),
        "bbox_max": None if box.IsEmpty() else list(box.GetMax()),
        "cameras": cameras,
        "render_products": products,
        "lights": lights,
    }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
