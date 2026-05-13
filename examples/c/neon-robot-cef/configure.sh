#!/usr/bin/env bash
# Configure neon-robot-cef on Linux. Downloads CEF Linux tarball (~300 MB)
# on first run. Requires (Ubuntu 24.04):
#   sudo apt install build-essential cmake glslang-tools \
#        libgtk-3-dev libnss3 libxss1 libasound2t64 libgbm1
# Optional but nice: ninja-build for faster incremental builds.
# Also needed on PATH: Vulkan SDK + CUDA Toolkit 12+.
set -euo pipefail
cd "$(dirname "$0")"

# Pick a generator we actually have. Ninja is preferred; fall back to
# Unix Makefiles so the script works on a fresh box without ninja-build.
if command -v ninja >/dev/null 2>&1; then
    GEN="Ninja"
else
    GEN="Unix Makefiles"
fi
echo "[configure] using generator: $GEN"

cmake -S . -B build -G "$GEN" -DCMAKE_BUILD_TYPE=Release "$@"
