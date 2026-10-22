#!/bin/bash
# Every file in tests/compiler/check_clean/ must be accepted by `zc check` (the
# full typechecker) with no errors, and must also build and run with `zc run`.
# These tests guard the typechecker fixes of the "single semantics" plan.
# Usage: tests/scripts/run_check_clean.sh [ZC]   (default ./zc)
ZC=${1:-./zc}
DIR=tests/compiler/check_clean
passed=0
failed=0
for f in "$DIR"/*.zc; do
    [ -e "$f" ] || continue
    out=$("$ZC" check "$f" 2>&1)
    if [ $? -ne 0 ]; then
        echo "FAIL (zc check) $f"
        printf '%s\n' "$out" | grep -A4 '^error' | sed 's/^/    /'
        failed=$((failed + 1))
        continue
    fi
    if ! "$ZC" run -w "$f" -o "${TMPDIR:-/tmp}/zc_check_clean_$$" >/dev/null 2>&1; then
        echo "FAIL (zc run)   $f"
        failed=$((failed + 1))
        continue
    fi
    passed=$((passed + 1))
done
rm -f "${TMPDIR:-/tmp}/zc_check_clean_$$"
echo "check_clean: $passed passed, $failed failed"
[ $failed -eq 0 ]
