#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["usd-core>=24.0"]
# ///
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary

"""Stand-alone USD inspector — runs in its own ephemeral venv.

ovrtx bundles its own USD C++ libraries and refuses to load when
`usd-core` is installed in the same environment. This script declares
its own deps via PEP 723 inline metadata so `uv run asset_bbox.py`
resolves it into a separate ephemeral venv; `main.py` shells out to it
and reads JSON back on stdout.

Reports, per asset: defaultPrim, upAxis, metersPerUnit and the
default-purpose world bounding box. `main.py` uses the bbox to auto-frame
the camera and to lay assets out without interpenetration.
"""

import json
import os
import sys

# The Commercial_NVD pack ships crate v0.7.0 files; silence the (harmless)
# deprecation spam so stdout stays pure JSON.
os.environ.setdefault("PXR_USDC_EMIT_DEPRECATION_WARNINGS", "0")

from pxr import Usd, UsdGeom  # noqa: E402


def inspect(path: str) -> dict:
    stage = Usd.Stage.Open(path)
    if stage is None:
        return {"path": path, "ok": False, "error": "Usd.Stage.Open returned None"}

    default_prim = stage.GetDefaultPrim()
    root = default_prim if default_prim else stage.GetPseudoRoot()
    cache = UsdGeom.BBoxCache(
        Usd.TimeCode.Default(),
        [UsdGeom.Tokens.default_, UsdGeom.Tokens.render],
    )
    rng = cache.ComputeWorldBound(root).ComputeAlignedRange()
    lo, hi = rng.GetMin(), rng.GetMax()

    return {
        "path": path,
        "ok": True,
        "default_prim": default_prim.GetPath().pathString if default_prim else None,
        "up_axis": UsdGeom.GetStageUpAxis(stage),
        "meters_per_unit": UsdGeom.GetStageMetersPerUnit(stage),
        "bbox_min": [lo[0], lo[1], lo[2]],
        "bbox_max": [hi[0], hi[1], hi[2]],
    }


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: asset_bbox.py <usd-file> [...]", file=sys.stderr)
        return 2
    json.dump([inspect(p) for p in sys.argv[1:]], sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
