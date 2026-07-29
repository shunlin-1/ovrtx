# Commercial Showroom

Renders the **Commercial_NVD** ArchVis asset pack with ovrtx.

> Fork-only example — not part of NVIDIA's upstream `ovrtx`.
> See [`../../EXPERIMENTS.md`](../../EXPERIMENTS.md).

## Why a sidecar stage is needed

The pack (`examples/assets/Commercial_NVD@10013/`) is a **library of props**, not a
renderable scene. Every `.usd` in it is a bare `/World` Xform:

| Property | Value |
|---|---|
| `defaultPrim` | `/World` (Xform) |
| `upAxis` | `Z` |
| `metersPerUnit` | `0.01` (centimetres) |
| Camera / lights / RenderProduct | **none** |
| Crate version | `0.7.0` (deprecated; USD still reads it, with a warning) |

ovrtx renders *RenderProducts*, so `main.py` generates a sidecar stage that
references the assets and supplies the missing camera, lights and RenderProduct,
then opens that. The sidecar matches the pack's `upAxis`/`metersPerUnit` exactly —
a metres-unit sidecar over a centimetre source puts the camera 100x too close.

Two details worth knowing:

- **The `@` in `Commercial_NVD@10013` breaks plain asset paths.** `@...@` is
  terminated by the first `@`, so references use Sdf's triple-delimiter form
  `@@@/path/with@sign.usd@@@`. See `asset_ref()`.
- **Asset bounds come from a separate venv.** ovrtx bundles its own USD libraries
  and refuses to load when `usd-core` is installed alongside it, so inspection
  lives in `asset_bbox.py`, which `uv run` resolves into its own ephemeral venv
  via PEP 723 inline metadata. `main.py` shells out and reads JSON back.

## Usage

```bash
uv run main.py --list                        # enumerate the 82 assets
uv run main.py                               # composed showroom scene
uv run main.py --asset Seating/Monarch.usd   # one asset, auto-framed
uv run main.py --dry-run                     # write the sidecar, skip rendering
```

Output goes to `build/` (gitignored): the PNG plus the generated `.usda`, which
is worth opening in usdview when framing looks wrong.

Useful flags: `--res WxH`, `--warmup N` (path-tracing accumulation steps),
`--azimuth` / `--elevation` / `--margin` (camera orbit and framing slack),
`--mode` (`omni:rtx:rendermode`), `--no-shell` (drop the floor and walls),
`--dome-intensity`.

If the pack lives elsewhere, point `--pack` at its
`.../Assets/ArchVis/Commercial` directory.

## Status

The sidecar generation and asset inspection are verified. **Rendering is
unverified** — the one attempt was killed by the OOM reaper, unsurprising given
the pack is 5.7 GB of textured geometry. Start with a single asset and a small
`--res` rather than the full showroom:

```bash
uv run main.py --asset Tables/OakTableSmall.usd --res 960x540 --warmup 16
```
