#!/bin/bash
# MISRA output-compliance harness.
#
# Transpiles Zen C sources to C and runs cppcheck's MISRA C:2012 addon on the
# generated code. This measures the *output*, not the source-level diagnostics
# covered by run_misra_tests.sh.
#
# Usage:
#   tests/scripts/run_misra_output_tests.sh [options] [--] [file.zc ...]
#
# Options:
#   --zc <path>     Compiler to use (default: ./zc, then ./zc.exe)
#   --max <n>       Exit non-zero when the total finding count exceeds <n>
#   --strict        Use --profile misra-c2012, the strict corpus, and
#                   -std=c99 -pedantic-errors compilation
#   --no-enable-all Only run cppcheck's default checks (faster)
#
# Environment:
#   MISRA_ADDON     Path to misra.py (auto-detected when omitted)
#   CPPCHECK        cppcheck executable (default: cppcheck)
#
# Without file arguments, a small representative corpus is used.

set -u

ZC="${ZC:-}"
MAX="${MAX_FINDINGS:-}"
ENABLE_ALL=1
STRICT=0
PROFILE=hosted
CC="${CC:-gcc}"

while [ $# -gt 0 ]; do
    case "$1" in
        --zc)
            ZC="$2"
            shift 2
            ;;
        --max)
            MAX="$2"
            shift 2
            ;;
        --strict)
            STRICT=1
            PROFILE=misra-c2012
            shift
            ;;
        --no-enable-all)
            ENABLE_ALL=0
            shift
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "Unknown option: $1" >&2
            exit 2
            ;;
        *)
            break
            ;;
    esac
done

if [ -z "$ZC" ]; then
    if [ -f ./zc ]; then
        ZC=./zc
    elif [ -f ./zc.exe ]; then
        ZC=./zc.exe
    else
        echo "Error: zc binary not found (use --zc)" >&2
        exit 1
    fi
fi

CPPCHECK="${CPPCHECK:-cppcheck}"
if ! command -v "$CPPCHECK" >/dev/null 2>&1; then
    echo "Error: cppcheck not found (install it or set CPPCHECK)" >&2
    exit 1
fi

if [ -z "${MISRA_ADDON:-}" ]; then
    cppcheck_dir="$(dirname "$(command -v "$CPPCHECK")")"
    for candidate in \
        "$cppcheck_dir/../share/cppcheck/addons/misra.py" \
        "$cppcheck_dir/../../share/cppcheck/addons/misra.py" \
        "/usr/share/cppcheck/addons/misra.py"
    do
        if [ -f "$candidate" ]; then
            MISRA_ADDON="$candidate"
            break
        fi
    done
fi
MISRA_ADDON="${MISRA_ADDON:-misra}"

FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then
    if [ "$STRICT" -eq 1 ]; then
        shopt -s nullglob
        FILES=(tests/misra_output/*.zc)
        shopt -u nullglob
    else
        FILES=(
            tests/language/basic/test_basics.zc
            tests/language/features/memory/test_drop.zc
            tests/language/features/fstrings/test_fstring.zc
            tests/language/control-flow/test_loops.zc
            tests/stdlib/collections/test_vec.zc
        )
    fi
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

CPPCHECK_ARGS=(
    "--addon=$MISRA_ADDON"
    --std=c99
    --inconclusive
    '-D__has_builtin(x)=0'
    '-D__has_feature(x)=0'
    --suppress=missingIncludeSystem
    --suppress=missingInclude
    --quiet
)
if [ "$ENABLE_ALL" -eq 1 ]; then
    CPPCHECK_ARGS+=(--enable=all)
fi

: > "$TMP/rules.txt"
total=0
deviations=0
failed_files=0
checked=0

for f in "${FILES[@]}"; do
    if [ ! -f "$f" ]; then
        echo "SKIP (missing): $f"
        continue
    fi

    base="$(basename "$f" .zc)"
    out="$TMP/${base}.c"
    # Both profiles suppress #line source mapping (Rule 20.13). The strict
    # profile also runs the MISRA source policy.
    if ! "$ZC" transpile "$f" --backend c --profile "$PROFILE" -o "$out" >"$TMP/stdout" 2>"$TMP/stderr"; then
        echo "FAIL (transpile): $f"
        sed -n '1,5p' "$TMP/stderr" | sed 's/^/    /'
        failed_files=$((failed_files + 1))
        continue
    fi

    checked=$((checked + 1))
    report="$TMP/report.txt"
    "$CPPCHECK" "${CPPCHECK_ARGS[@]}" "$out" >"$report" 2>&1

    if grep -q "Failed to auto detect python" "$report"; then
        echo "Error: cppcheck could not find Python for the addon." >&2
        echo "       Put python on PATH (e.g. export PATH=/ucrt64/bin:\$PATH)." >&2
        exit 1
    fi

    n=$(grep -cE 'misra-c2012-[0-9]+\.[0-9]+' "$report" || true)
    dev=0
    if [ "$STRICT" -eq 1 ]; then
        # Rule 8.7 on main is the one documented exception: the C entry point
        # must have external linkage. Internal helpers and globals are static.
        dev=$(grep -cE 'misra-c2012-8\.7' "$report" || true)
        deviations=$((deviations + dev))
        n=$((n - dev))
        grep -oE 'misra-c2012-[0-9]+\.[0-9]+' "$report" | grep -vE '8\.7' \
            >> "$TMP/rules.txt" || true
        if ! "$CC" -std=c99 -pedantic-errors -c "$out" -o "$out.o" >"$TMP/cc" 2>&1; then
            echo "FAIL (C99 compile): $f"
            sed -n '1,5p' "$TMP/cc" | sed 's/^/    /'
            failed_files=$((failed_files + 1))
        fi
    else
        grep -oE 'misra-c2012-[0-9]+\.[0-9]+' "$report" >> "$TMP/rules.txt" || true
    fi
    total=$((total + n))
    printf '%-60s %4d MISRA finding(s)' "$f" "$n"
    if [ "$dev" -gt 0 ]; then
        printf ' (+%d documented 8.7/main)' "$dev"
    fi
    printf '\n'

    # Non-MISRA findings are usually parse/setup problems; surface them.
    if grep -qE '\[(syntaxError|internalError|unknownMacro|preprocessorError)\]' "$report"; then
        echo "    note: cppcheck reported a parse/setup problem:"
        grep -E '\[(syntaxError|internalError|unknownMacro|preprocessorError)\]' "$report" |
            sed -n '1,3p' | sed 's/^/    /'
    fi
done

echo ""
echo "Checked $checked file(s), $failed_files transpile/compile failure(s)."
echo "Total MISRA findings: $total"
if [ "$STRICT" -eq 1 ]; then
    echo "Documented deviations: $deviations (Rule 8.7 on main)"
fi
if [ -s "$TMP/rules.txt" ]; then
    echo ""
    echo "Findings by rule:"
    sort "$TMP/rules.txt" | uniq -c | sort -nr | sed 's/^/  /'
fi

if [ -n "$MAX" ] && [ "$total" -gt "$MAX" ]; then
    echo ""
    echo "FAIL: $total findings exceed the limit of $MAX"
    exit 1
fi

if [ "$failed_files" -ne 0 ]; then
    exit 1
fi

exit 0
