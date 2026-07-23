#!/usr/bin/env bash
#
# V8.6c / V8.10 — layering guard.
#
# V8 rule: nothing under src/core/ may include a ts/ or ui/ header.
# As of V8.10 the legacy allowlist is empty — core/ is pure again.
#
# Usage: bash tests/check_layering.sh   (exit != 0 on any violation)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ALLOWLIST=()

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
