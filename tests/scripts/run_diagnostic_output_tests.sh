#!/bin/bash
# Diagnostic output tests: compiles each tests/compiler/diagnostics_output/*.zc with the
# arguments of its first line (`// ARGS: <command> [options]`) and compares the output and
# the exit code with <name>.expected.
#
# Usage: tests/scripts/run_diagnostic_output_tests.sh [ZC]
#   UPDATE=1 rewrites the .expected files with the current output.

cd "$(dirname "$0")/../.." || exit 1
ZC="${1:-./zc}"
DIR="tests/compiler/diagnostics_output"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

for src in "$DIR"/*.zc; do
    name="${src%.zc}"
    args=$(sed -n '1s|^// ARGS: ||p' "$src")
    command=${args%% *}
    options=""
    if [ "$args" != "$command" ]; then
        options=${args#* }
    fi

    # Colours are off when stderr is not a terminal; build times vary between runs.
    # shellcheck disable=SC2086
    "$ZC" "$command" "$src" $options -o "$TMP/out" > "$TMP/actual" 2>&1
    echo "[exit $?]" >> "$TMP/actual"
    sed -E 's/ in [0-9.]+s / in Xs /' "$TMP/actual" > "$TMP/normalized"
    if [ "${UPDATE:-0}" = "1" ]; then
        cp "$TMP/normalized" "$name.expected"
        echo "updated: $name.expected"
    elif diff -u "$name.expected" "$TMP/normalized" > "$TMP/diff"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $src"
        cat "$TMP/diff"
    fi
done

if [ "${UPDATE:-0}" != "1" ]; then
    echo "Diagnostic output tests: $pass passed, $fail failed"
fi
[ "$fail" -eq 0 ]
