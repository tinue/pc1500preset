#!/usr/bin/env bash
# Builds both this project's own tool and the emulator, each with its own
# independent CMake tree -- see the top-level README's "Building" section
# for why these are never combined into a single build.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "==> Building pc1500preset"
cmake -B build
cmake --build build -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "==> Building ../pc1500emu"
cmake -B ../pc1500emu/build -S ../pc1500emu
cmake --build ../pc1500emu/build -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
