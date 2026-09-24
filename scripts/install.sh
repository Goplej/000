#!/bin/sh
# ==============================================================================
#  local-claude-code-agent -- installation script
#
#  Builds the Release binary and installs it to $PREFIX/bin/lca
#  (default PREFIX: $HOME/.local).
#
#  Usage:
#    ./scripts/install.sh                 # build + install to ~/.local/bin
#    PREFIX=/opt/lca ./scripts/install.sh # custom prefix
#    ./scripts/install.sh --build-only    # just build ./build/lca
#    ./scripts/install.sh --run-tests     # build, run the test suite, install
# ==============================================================================
set -eu

usage() {
    sed -n '3,16p' "$0" | sed 's/^#  \{0,1\}//'
    exit "${1:-0}"
}

BUILD_ONLY=0
RUN_TESTS=0
for arg in "$@"; do
    case "$arg" in
        -h|--help)     usage 0 ;;
        --build-only)  BUILD_ONLY=1 ;;
        --run-tests)   RUN_TESTS=1 ;;
        *) echo "unknown option: $arg" >&2; usage 2 ;;
    esac
done

# Always operate from the repository root (the parent of scripts/).
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$ROOT"

PREFIX="${PREFIX:-$HOME/.local}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-}"

if [ -z "$JOBS" ]; then
    if command -v nproc >/dev/null 2>&1; then JOBS=$(nproc)
    else JOBS=4; fi
fi

echo "==> local-claude-code-agent installer"
echo "    root   : $ROOT"
echo "    prefix : $PREFIX"
echo "    build  : $BUILD_TYPE (-j$JOBS)"

have() { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------------------------------- build --
if have cmake; then
    echo "==> configuring with CMake"
    cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    echo "==> building"
    cmake --build build -j "$JOBS"
    BIN=build/lca
elif have make && { have c++ || have g++ || have clang++; }; then
    echo "==> CMake not found; building with GNU Make"
    make -j "$JOBS"
    BIN=build/lca
else
    echo "error: need either cmake, or make plus a C++17 compiler" >&2
    exit 1
fi

if [ ! -x "$BIN" ]; then
    echo "error: build finished but $BIN is missing" >&2
    exit 1
fi
echo "==> built $BIN ($(wc -c < "$BIN") bytes)"

"$BIN" version

# ---------------------------------------------------------------------- tests --
if [ "$RUN_TESTS" -eq 1 ]; then
    echo "==> running test suite"
    "$SCRIPT_DIR/run_tests.sh"
fi

# -------------------------------------------------------------------- install --
if [ "$BUILD_ONLY" -eq 1 ]; then
    echo "==> --build-only: leaving $BIN in place"
    exit 0
fi

echo "==> installing to $PREFIX/bin/lca"
mkdir -p "$PREFIX/bin"
if have install; then
    install -m 0755 "$BIN" "$PREFIX/bin/lca"
else
    cp "$BIN" "$PREFIX/bin/lca"
    chmod 0755 "$PREFIX/bin/lca"
fi

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo "note: $PREFIX/bin is not on PATH; add:  export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac

echo "==> running doctor"
"$PREFIX/bin/lca" doctor || true

echo "==> done. Try:  lca run \"create a python project called hello_cli\""
