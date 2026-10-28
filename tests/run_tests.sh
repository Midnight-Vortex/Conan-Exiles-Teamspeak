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
    src/core/proximity/proximity_math.c \
    src/core/proximity/occlusion_params.c

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

# 077 — ingame channel password delivered by the game mod (cut, store, TTL, no log).
run_suite mod_password_test \
    -Itests/support/win32_shim \
    tests/mod_password_test.c \
    tests/support/win32_shim/win32_shim.c \
    src/core/hub/mod_password.c

# 061 PROTOTYPE — occlusion values from the mod (parse, stale, apply, probes).
run_suite occlusion_test \
    -Itests/support/win32_shim \
    tests/occlusion_test.c \
    tests/support/win32_shim/win32_shim.c \
    src/core/proximity/occlusion.c \
    src/core/proximity/occlusion_params.c \
    src/core/proximity/player_table.c \
    src/core/proximity/proximity_math.c

# V8.3 — pure PCM-ownership decision helper (render_state_needs_reinit).
# Includes ts3_proximity_audio.h only; the TS SDK header needs -Isdk/include.
run_suite render_state_test \
    -Isdk/include \
    tests/render_state_test.c

# V8.4 — pure wakeup coalescing/rate-limit decision (wakeup_should_send).
# Header-only, no Win32/TS coupling.
run_suite wakeup_policy_test \
    tests/wakeup_policy_test.c

# V8.14 — CEMODE wire codec (meters->decimeters, format, strict parse).
# Header-only in ts3_cemode_wire.h, pure C, no Win32/TS coupling.
run_suite cemode_wire_test \
    tests/cemode_wire_test.c

# V8.15 — CEPING wire codec (format, strict parse, wraparound-safe seq gap).
# Header-only in ts3_ceping_wire.h, pure C, no Win32/TS coupling.
run_suite ceping_wire_test \
    tests/ceping_wire_test.c

run_suite ceauth_wire_test \
    tests/ceauth_wire_test.c

# V8.4 — typed command ring (control-plane channel B). Pure mechanics in
# ts3_cmd_ring.h; needs -Isdk/include for the Ts3Command type (SDK uint64).
run_suite cmd_queue_test \
    -Isdk/include \
    tests/cmd_queue_test.c

# V8.6 — voice_modes mode->distance decision, now host-testable after the
# ts/ui hook inversion. Uses the Win32 shim (hotkey helpers) + real zone_resolve.
run_suite voice_modes_test \
    -Itests/support/win32_shim \
    tests/voice_modes_test.c \
    tests/support/win32_shim/win32_shim.c \
    src/core/voice/voice_modes.c \
    src/core/proximity/zone_resolve.c

# V8.8 — CEPOS/player-table load simulation (200-player baseline + 600 eviction).
run_suite cepos_load_test \
    -Itests/support/win32_shim \
    tests/cepos_load_test.c \
    tests/support/win32_shim/win32_shim.c \
    src/core/proximity/player_table.c \
    src/core/proximity/proximity_math.c

# 062 — binaural HRTF (Brown & Duda structural model).
run_suite hrtf_test \
    tests/hrtf_test.c \
    src/core/proximity/hrtf.c

# 067 — talk status for the mod's mouth animation.
run_suite talk_state_test \
    -Itests/support/win32_shim \
    tests/talk_state_test.c \
    tests/support/win32_shim/win32_shim.c \
    src/core/proximity/talk_state.c \
    src/core/proximity/player_table.c \
    src/core/proximity/proximity_math.c

# 085 — HTTP request helpers (header parsing, browser guard, Content-Length, throttle, backoff).
run_suite http_request_test \
    tests/http_request_test.c

# 085 — Pos.txt / legacy HTTP line parser + sample sanity (header-only pos_parse.h).
run_suite pos_parse_test \
    tests/pos_parse_test.c

# 085 — plugin.cfg text parse / clamp (NaN/Inf) / UTF-8 decision (header-only config_text.h).
run_suite config_text_test \
    tests/config_text_test.c

# 083 — CEPOS received-name sanitising (pure wire helper).
run_suite cepos_wire_test \
    tests/cepos_wire_test.c

# 083 — real ts3_cemode/ceping/cepos/3d against stubs + fake clock.
# -iquote makes the shim plugin.h win over src/plugin.h; GetTickCount64 is defined in the test.
run_suite proto_flush_test \
    -iquote tests/support/proto_shim \
    -Itests/support/win32_shim \
    -Isdk/include \
    -include tests/support/proto_shim/proto_compat.h \
    tests/proto_flush_test.c \
    src/ts/proximity/ts3_cemode.c \
    src/ts/proximity/ts3_ceping.c \
    src/ts/proximity/ts3_cepos.c \
    src/ts/proximity/ts3_3d.c \
    src/core/proximity/proximity_math.c

# 088 — multi-producer deferred-chat ring (real spin lock + ring, pthreads).
run_suite ui_chat_ring_test \
    tests/ui_chat_ring_test.c

# 088 — start/stop worker-handle hand-off (overlay monitor thread), pthreads.
run_suite ui_worker_slot_test \
    tests/ui_worker_slot_test.c

# V8.6c — layering guard (no ts/ or ui/ include under src/core/).
echo "=== layering_guard ==="
if bash tests/check_layering.sh; then
    RESULTS+=("PASS  layering_guard")
else
    RESULTS+=("FAIL  layering_guard")
    FAILED=1
fi
echo

echo "=== summary ==="
for r in "${RESULTS[@]}"; do
    echo "$r"
done

if [ "$FAILED" -ne 0 ]; then
    echo "RESULT: FAILED"
    exit 1
fi
echo "RESULT: ALL SUITES PASSED"
