#!/usr/bin/env bash
#
# V8.6c — layering guard.
#
# V8 rule (doku/01-architektur-v8.md, "Schichten-Modell"): nothing under
# src/core/ may include a ts/ or ui/ header. core/ is PURE logic and must stay
# unit-testable on any host. SDK type headers (teamspeak/public_definitions.h)
# are NOT our ts/ layer and are allowed.
#
# This guard's job is to block *new* violations. A documented allowlist covers
# the files that still violate today and are scheduled to be fixed elsewhere.
# It runs in the CI gate (tests/run_tests.sh).
#
# Usage: bash tests/check_layering.sh   (exit != 0 on a new violation)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Files under src/core/ that STILL include a ts/ or ui/ header today.
# Legacy Mumble blob — the F10-save persistence bridge (config_files.c) and the
# shared base utils (util_base.c) still reference ui/ts symbols; full relocation
# is tracked for V8.6/V8.7. proximity_volume.c was deleted and validation.c was
# decoupled from ui/ts in V8.5b (doku/019-legacy-abbau.md); nick_anonymize.c was
# relocated into ts/ in V8.6 (doku/021-nick-nach-ts.md), exactly like
# channel_manage was moved in V8.6a (doku/015).
ALLOWLIST=(
    "src/core/config/config_files.c"       # legacy F10-save bridge, relocation tracked for V8.6
    "src/core/util/util_base.c"            # legacy shared utils, relocation tracked for V8.6
)

INCLUDE_RE='^[[:space:]]*#[[:space:]]*include[[:space:]]*"(ts|ui)/'

is_allowed() {
    local f="$1" a
    for a in "${ALLOWLIST[@]}"; do
        [ "$a" = "$f" ] && return 0
    done
    return 1
}

echo "=== layering guard: core/ must not include ts/ or ui/ ==="

hits=()
while IFS= read -r line; do
    [ -n "$line" ] && hits+=("$line")
done < <(grep -rlE "$INCLUDE_RE" src/core --include='*.c' --include='*.h' 2>/dev/null | sort -u || true)

new_violations=0
known=0
for f in "${hits[@]:-}"; do
    [ -n "$f" ] || continue
    if is_allowed "$f"; then
        echo "  known (allowed): $f"
        known=$((known + 1))
    else
        echo "  NEW VIOLATION:   $f"
        grep -nE "$INCLUDE_RE" "$f" | sed 's/^/        /'
        new_violations=$((new_violations + 1))
    fi
done

# Keep the allowlist honest: warn (do not fail) when an entry no longer
# violates — e.g. after the V8.5b legacy blob removal it should be pruned.
for a in "${ALLOWLIST[@]}"; do
    found=0
    for f in "${hits[@]:-}"; do
        [ "$f" = "$a" ] && found=1 && break
    done
    if [ "$found" -eq 0 ]; then
        echo "  stale allowlist entry (no longer violates — please remove): $a"
    fi
done

echo "  summary: ${known} known-allowed, ${new_violations} new"
if [ "$new_violations" -ne 0 ]; then
    echo "LAYERING: FAILED — a core/ file gained a ts/ or ui/ include"
    exit 1
fi
echo "LAYERING: OK"
