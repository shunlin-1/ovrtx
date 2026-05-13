#!/usr/bin/env bash
# Launch neon-robot-cef. Pass through any extra args (e.g. --url, --usd).
set -euo pipefail
cd "$(dirname "$0")/build"
exec ./neon-robot-cef "$@"
