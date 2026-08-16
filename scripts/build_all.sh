#!/usr/bin/env bash
# Builds both this project's own tool and the vendored emulator, each with
# its own independent CMake tree -- see the top-level README's "Building"
# section for why these are never combined into a single build.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "==> Building pc1500preset"
cmake -B build
cmake --build build -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "==> Building vendored pc1500emu"
cmake -B vendor/pc1500emu/build -S vendor/pc1500emu
cmake --build vendor/pc1500emu/build -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
