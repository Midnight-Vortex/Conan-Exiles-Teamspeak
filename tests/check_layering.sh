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
# Two documented groups (see doku/aenderungen/017-layering-wache.md):
#   1) legacy Mumble blob — dies with V8.5b (config parsing / validation /
#      base utils / volume math still cross-linked into the old plugin.c world)
#   2) nick_anonymize.c — fully TS-callback-coupled (calls ts3_* directly);
#      relocation into ts/ is tracked for a later V8.6 sub-package, exactly
#      like channel_manage was moved in V8.6a.
ALLOWLIST=(
    "src/core/config/config_files.c"       # legacy, removed in V8.5b
    "src/core/validation/validation.c"     # legacy, removed in V8.5b
    "src/core/util/util_base.c"            # legacy, removed in V8.5b
    "src/core/proximity/proximity_volume.c" # legacy, removed in V8.5b
    "src/core/nick/nick_anonymize.c"       # TS-coupled, relocation to ts/ tracked
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
