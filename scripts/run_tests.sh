#!/bin/sh
# ==============================================================================
#  local-claude-code-agent -- build and run every test suite
#
#  Usage:
#    ./scripts/run_tests.sh            # build + run all suites
#    ./scripts/run_tests.sh test_buf   # run a single suite
#
#  Suites that need `openssl` or `python3` SKIP themselves at runtime when the
#  tool is missing, so the script stays green on minimal systems.
# ==============================================================================
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$ROOT"

"$SCRIPT_DIR/build.sh" >/dev/null

SUITES="test_crypto test_buf test_fs_engine test_search test_tls_fixture test_sandbox"
if [ "$#" -gt 0 ]; then
    SUITES="$*"
fi

fail=0
for t in $SUITES; do
    printf '%-18s ' "$t"
    if [ -x "build/$t" ]; then
        bin="build/$t"
    elif [ -x "build/tests/$t" ]; then
        bin="build/tests/$t"          # CMake layout
    else
        # Build just this suite (make fallback or CMake).
        if command -v cmake >/dev/null 2>&1 && [ -f build/CMakeCache.txt ]; then
            cmake --build build --target "$t" >/dev/null 2>&1 || true
        else
            make "build/$t" >/dev/null 2>&1 || true
        fi
        if [ -x "build/$t" ]; then
            bin="build/$t"
        elif [ -x "build/tests/$t" ]; then
            bin="build/tests/$t"
        else
            echo "MISSING (could not build)"
            fail=1
            continue
        fi
    fi
    if timeout 600 "$bin" > "build/$t.log" 2>&1; then
        grep -E "passed|checks," "build/$t.log" | tail -n 1 || tail -n 1 "build/$t.log"
    else
        echo "FAILED (see build/$t.log)"
        fail=1
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "all suites green"
else
    echo "some suites failed" >&2
fi
exit "$fail"
