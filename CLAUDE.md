# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The canonical agent guide is `AGENTS.md` — read it for repo purpose, layout, skill map, common Python/C workflows, and the mandatory snippet/skill rules.

@AGENTS.md

## Fork-specific addendum (not upstream)

This checkout is the `shunlin-1/ovrtx` fork. The examples below are fork-only R&D and are **not** part of NVIDIA's upstream `ovrtx`; AGENTS.md deliberately does not mention them. The orientation index for everything here is `examples/EXPERIMENTS.md` — read it before touching any of these directories. Each project's own `README.md` is the source of truth for build/run details.

- Python (ovrtx-rendered viewers): `examples/python/agv/`, `examples/python/neon-robot/`, `examples/python/neon-robot-bim/`, `examples/python/commercial-showroom/`
- Hybrid C++ UI + Python helpers: `examples/hybrid/agv/` (Qt6/QML + ovrtx C API, uv-run subprocesses for USD / AI work)
- C/C++ (UI-embedding spikes): `examples/c/neon-robot-c/`, `examples/c/neon-robot-cef/`, `examples/c/cef-bim-test/`, `examples/c/cef-panel/`, `examples/c/cef-thread-spike/`, `examples/c/ultralight-test/`
- Web (overlay HTML used by the CEF/Ultralight spikes): `examples/web/ui-demo/`

CEF-based C++ projects auto-download the CEF binary distribution (~250 MB) on first configure. Ultralight expects the SDK installed locally.

### ovrtx version and scene-ownership mode

The fork examples target **ovrtx 0.4** but stay in **standalone mode**: the
renderer still owns the stage, via `open_usd()` / `add_usd_reference_from_string()` /
`bind_attribute()` / `map_attribute()` in Python and `ovrtx_open_usd_from_file()` in C.
Those APIs are deprecated in 0.4 and emit `DeprecationWarning` (Python) or
`-Wdeprecated-declarations` (C) — that is expected here, not a defect to
"fix" by suppressing the warning.

The 0.4 direction is to move scene data to **ovstage 0.1** (`ovstage.Stage`,
`ovstage.population.*`, ordinal-keyed writes and write-floor advances) and
attach it with `Renderer.attach_ovstage()`. Upstream's own `examples/python/minimal`
and `examples/c/minimal` are already written that way and are the reference if
you migrate a fork example. Use `skills/update-0_3-0_4-python/SKILL.md` and
`skills/update-0_3-0_4-c/SKILL.md` for that step.

## Running the Python test suite

Unlike the examples (self-contained `uv run` projects), `tests/` runs against an installed/packaged ovrtx and needs the native library on the loader path:

- Linux: `LD_LIBRARY_PATH` must include the directory holding `libovrtx-dynamic.so`.
- Windows: `PATH` must include the directory holding `ovrtx-dynamic.dll`.
- If `ovrtx` is not `pip install`-ed, also set `PYTHONPATH` to the parent of the `ovrtx` package.

Install deps with `pip install -r tests/requirements.txt`, then:

```bash
pytest tests/                                    # full suite
pytest tests/ -k test_renderer                   # one test
pytest tests/ --test-data=/path/to/usd/scenes    # override USD fixture path
pytest tests/ --output=/path/to/output           # redirect logs/dumps (default tests/_output/)
```

Full troubleshooting (CUDA errors, missing test data, import errors) is in `tests/README.md`.

## Building the docs

```bash
cd docs && make html
uv run python -m http.server 8000 -d _build/html   # then open http://localhost:8000/
```

Requires `uv` and `doxygen`. Sphinx extras are declared under `[project.optional-dependencies].docs` in `pyproject.toml`.
