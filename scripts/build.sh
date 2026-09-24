#!/bin/sh
# ==============================================================================
#  local-claude-code-agent -- build only (CMake when present, else GNU Make)
#
#  Usage:
#    ./scripts/build.sh            # Release build into ./build/lca
#    ./scripts/build.sh Debug      # other CMAKE_BUILD_TYPE values
#    JOBS=2 ./scripts/build.sh     # cap parallelism
# ==============================================================================
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$ROOT"

BUILD_TYPE="${1:-Release}"
JOBS="${JOBS:-}"
if [ -z "$JOBS" ]; then
    if command -v nproc >/dev/null 2>&1; then JOBS=$(nproc)
    else JOBS=4; fi
fi

if command -v cmake >/dev/null 2>&1; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    cmake --build build -j "$JOBS"
else
    make -j "$JOBS" all tests
fi

echo "==> $(./build/lca version)  ->  $ROOT/build/lca"
