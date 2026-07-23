#!/usr/bin/env bash
#
# V8.0/V8.1 — host unit tests for the pure core modules.
#
# Compiles and runs every suite with plain gcc on Linux (no TS SDK, no Win32).
# player_table.c builds against the test-only Win32 shim in
# tests/support/win32_shim/. Prints PASS/FAIL per suite, exit != 0 on failure.
#
# Usage: bash tests/run_tests.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT="tests/out"
mkdir -p "$OUT"

CFLAGS=(-O2 -Wall -Wextra -I. -Isrc)

FAILED=0
RESULTS=()

run_suite() {
    local name="$1"
    shift
    echo "=== $name ==="
    if ! gcc "${CFLAGS[@]}" "$@" -o "$OUT/$name" -lm -lpthread; then
        echo "  compile FAILED"
        RESULTS+=("FAIL  $name (compile)")
        FAILED=1
        return
    fi
    if "$OUT/$name"; then
        RESULTS+=("PASS  $name")
    else
        RESULTS+=("FAIL  $name")
        FAILED=1
    fi
    echo
}

run_suite hub_parser_test \
    tests/hub_parser_test.c \
    src/core/hub/hub_parser.c \
    src/core/proximity/zone_resolve.c \
    src/core/proximity/proximity_math.c

run_suite proximity_math_test \
    tests/proximity_math_test.c \
    src/core/proximity/proximity_math.c

run_suite zone_resolve_test \
    tests/zone_resolve_test.c \
    src/core/proximity/zone_resolve.c

run_suite player_table_test \
    -Itests/support/win32_shim \
    tests/player_table_test.c \
    tests/support/win32_shim/win32_shim.c \
    src/core/proximity/player_table.c

# V8.3 — pure PCM-ownership decision helper (render_state_needs_reinit).
# Includes ts3_proximity_audio.h only; the TS SDK header needs -Isdk/include.
run_suite render_state_test \
    -Isdk/include \
    tests/render_state_test.c

# V8.4 — pure wakeup coalescing/rate-limit decision (wakeup_should_send).
# Header-only, no Win32/TS coupling.
run_suite wakeup_policy_test \
    tests/wakeup_policy_test.c

echo "=== summary ==="
for r in "${RESULTS[@]}"; do
    echo "$r"
done

if [ "$FAILED" -ne 0 ]; then
    echo "RESULT: FAILED"
    exit 1
fi
echo "RESULT: ALL SUITES PASSED"
