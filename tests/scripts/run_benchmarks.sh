#!/bin/bash

# Zen-C Benchmarking Script
# Runs N iterations of each benchmark, reports median + MAD.
# Outputs JSON format compatible with github-action-benchmark.

ITERATIONS=5
ZC="./zc"
if [ ! -f "$ZC" ]; then
    ZC="./build/zc"
fi
if [ ! -f "$ZC" ]; then
    echo "Error: zc binary not found."
    exit 1
fi

RESULTS="[]"

add_result() {
    local name="$1"
    local value="$2"
    local unit="$3"
    local entry="{\"name\": \"$name\", \"value\": $value, \"unit\": \"$unit\"}"
    if [ "$RESULTS" == "[]" ]; then
        RESULTS="[$entry]"
    else
        RESULTS="${RESULTS%]}, $entry]"
    fi
}

# Compute median from sorted list of numbers
median_of() {
    local arr=("$@")
    local len=${#arr[@]}
    local mid=$((len / 2))
    if ((len % 2 == 0)); then
        echo $(( (arr[mid-1] + arr[mid]) / 2 ))
    else
        echo "${arr[mid]}"
    fi
}

# Compute median absolute deviation
mad_of() {
    local arr=("$@")
    local med="$1"
    local devs=()
    for v in "${arr[@]}"; do
        d=$(( v > med ? v - med : med - v ))
        devs+=("$d")
    done
    mapfile -t devs_sorted < <(printf '%s\n' "${devs[@]}" | sort -n)
    local len=${#devs_sorted[@]}
    local mid=$((len / 2))
    if ((len % 2 == 0)); then
        echo $(( (devs_sorted[mid-1] + devs_sorted[mid]) / 2 ))
    else
        echo "${devs_sorted[mid]}"
    fi
}

# Runs a command with its output discarded and prints how many milliseconds it took, on a
# monotonic clock: `date` follows the wall clock, which can jump backwards and gave negative
# times. Exits with the command's status.
time_ms() {
    python3 -c '
import subprocess, sys, time
start = time.monotonic()
code = subprocess.call(sys.argv[1:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(int((time.monotonic() - start) * 1000))
sys.exit(code)
' "$@"
}

echo "=== Compiler Benchmark: Full Suite Transpilation ==="
FILE_COUNT=$(find tests -name "*.zc" -not -name "_*.zc" | wc -l)
for _ in $(seq 1 "$ITERATIONS"); do
    # Some tests are meant not to compile: the status of the whole run does not matter.
    comp_times+=("$(time_ms bash -c 'find tests -name "*.zc" -not -name "_*.zc" -print0 |
        xargs -0 -L 1 "$0" transpile -o .bench_comp_temp.c -w' "$ZC")")
done
rm -f .bench_comp_temp.c
mapfile -t comp_sorted < <(printf '%s\n' "${comp_times[@]}" | sort -n)
COMP_MED=$(median_of "${comp_sorted[@]}")
COMP_MAD=$(mad_of "${comp_sorted[@]}")
COMP_AVG=$((COMP_MED / FILE_COUNT))
echo "Compiler: median=$COMP_MED ms, MAD=$COMP_MAD ms ($FILE_COUNT files, avg $COMP_AVG ms/file)"
add_result "Compiler (Full Suite Transpilation)" "$COMP_MED" "ms"

echo ""
echo "=== Runtime Benchmarks ==="
echo "(Warmup + $ITERATIONS iterations, reporting median)"

for bench in tests/bench/*.zc; do
    name=$(basename "$bench" .zc)
    echo -n "Compiling $name... "

    if ! COMP_TIME=$(time_ms "$ZC" build "$bench" -o "bench_$name" -w); then
        echo "FAILED"
        continue
    fi
    echo "compiled in ${COMP_TIME}ms"

    # Warmup run (discarded)
    "./bench_$name" > /dev/null 2>&1

    # Measure N iterations
    results=()
    echo -n "  runs:"
    for _ in $(seq 1 "$ITERATIONS"); do
        output=$("./bench_$name" 2>/dev/null)
        val=$(echo "$output" | grep "RESULT:" | cut -d' ' -f2)
        if [ -n "$val" ]; then
            results+=("$val")
            echo -n " $val"
        fi
    done
    echo ""

    if [ ${#results[@]} -eq 0 ]; then
        echo "  No valid results for $name"
        rm "bench_$name"
        continue
    fi

    mapfile -t sorted < <(printf '%s\n' "${results[@]}" | sort -n)
    MED=$(median_of "${sorted[@]}")
    MAD=$(mad_of "${sorted[@]}")
    echo "  median=$MED ms, MAD=$MAD ms"
    add_result "Runtime ($name)" "$MED" "ms"
    add_result "Compile ($name)" "$COMP_TIME" "ms"

    rm "bench_$name"
done

echo "$RESULTS" > benchmarks_result.json
echo ""
echo "Results saved to benchmarks_result.json"
