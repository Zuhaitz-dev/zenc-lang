#!/bin/bash
# Zen C CLI integration tests.
#
# Verifies the top-level `zc` command surface: version output, usage/help,
# and error handling for bad invocations, plus a compile+run smoke test.
#
# Usage: ./tests/scripts/run_cli_tests.sh
# Env:   ZC=path/to/zc          (default: ./zc)
#        ZC_ROOT=path/to/stdlib (set when running from outside the repo)

ZC="${ZC:-./zc}"
if [ ! -f "$ZC" ]; then
    echo "Error: zc binary not found ($ZC). Build it first." >&2
    exit 1
fi

PASSED=0
FAILED=0
FAILED_TESTS=""

check() {
    local name="$1"
    local ok="$2"
    if [ "$ok" = "1" ]; then
        echo "  [PASS] $name"
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] $name"
        FAILED=$((FAILED + 1))
        FAILED_TESTS="$FAILED_TESTS\n- $name"
    fi
}

# --- Version ---------------------------------------------------------------

out=$("$ZC" --version 2>&1)
if echo "$out" | grep -qE '^zc v[0-9]'; then
    check "--version prints version" 1
else
    check "--version prints version" 0
fi

out=$("$ZC" -V 2>&1)
if echo "$out" | grep -qE '^zc v[0-9]'; then
    check "-V prints version" 1
else
    check "-V prints version" 0
fi

# --- Usage / help ----------------------------------------------------------

out=$("$ZC" 2>&1)
if echo "$out" | grep -q "usage: zc"; then
    check "no arguments prints usage" 1
else
    check "no arguments prints usage" 0
fi

out=$("$ZC" help 2>&1)
if echo "$out" | grep -q "usage: zc"; then
    check "zc help prints usage" 1
else
    check "zc help prints usage" 0
fi

# --- Error paths -----------------------------------------------------------

out=$("$ZC" run 2>&1)
if echo "$out" | grep -q "no input file specified"; then
    check "zc run with no file reports error" 1
else
    check "zc run with no file reports error" 0
fi

out=$("$ZC" run "$ZC/does_not_exist.zc" 2>&1)
if echo "$out" | grep -q "could not read file"; then
    check "zc run with missing file reports error" 1
else
    check "zc run with missing file reports error" 0
fi

out=$("$ZC" --definitely-not-a-flag "$ZC" 2>&1)
if echo "$out" | grep -qiE "unknown|unrecognized|error"; then
    check "unknown flag reports error" 1
else
    check "unknown flag reports error" 0
fi

# --- Compile + run smoke test ----------------------------------------------

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
printf 'fn main() {\n    println "cli-smoke-ok"\n}\n' > "$workdir/smoke.zc"

out=$("$ZC" run "$workdir/smoke.zc" -o "$workdir/smoke" 2>&1)
if echo "$out" | grep -q "cli-smoke-ok"; then
    check "zc run executes a program" 1
else
    check "zc run executes a program" 0
fi

out=$("$ZC" transpile "$workdir/smoke.zc" -o "$workdir/smoke_out.c" 2>&1)
if [ -s "$workdir/smoke_out.c" ]; then
    check "zc transpile emits C source" 1
else
    check "zc transpile emits C source" 0
fi

# --- Program output ----------------------------------------------------------

# Runs a program and compares its exact standard output (stdin from $3).
output_check() {
    local name="$1"
    local code="$2"
    local input="$3"
    local expected="$4"
    printf '%s\n' "$code" > "$workdir/output.zc"
    "$ZC" build -w "$workdir/output.zc" -o "$workdir/output_bin" >/dev/null 2>&1
    local actual
    actual=$(printf '%s' "$input" | "$workdir/output_bin" 2>/dev/null)
    if [ "$actual" = "$expected" ]; then
        check "$name" 1
    else
        check "$name" 0
    fi
}

output_check "bare strings print without semicolons" 'fn main() {
    let x = 3
    "first"
    "x is {x}"
    "last"
}' "" "first
x is 3
last"

out=$("$ZC" build "$workdir/smoke.zc" -o "$workdir/smoke_build" 2>&1)
if [ -x "$workdir/smoke_build" ]; then
    check "zc build compiles a program" 1
else
    check "zc build compiles a program" 0
fi

out=$("$ZC" "$workdir/smoke.zc" -o "$workdir/smoke_implicit" 2>&1)
if [ -x "$workdir/smoke_implicit" ]; then
    check "zc <file> builds without a command" 1
else
    check "zc <file> builds without a command" 0
fi

out=$("$ZC" frobnicate "$workdir/smoke.zc" -o "$workdir/smoke_unknown" 2>&1)
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "unknown command 'frobnicate'" &&
    [ ! -e "$workdir/smoke_unknown" ]; then
    check "unknown command reports error" 1
else
    check "unknown command reports error" 0
fi

out=$("$ZC" transpile "$workdir/smoke.zc" --backend astdump -o "$workdir/smoke_bad_backend" 2>&1)
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "unknown backend 'astdump'" &&
    echo "$out" | grep -q "did you mean 'ast-dump'" && [ ! -e "$workdir/smoke_bad_backend" ]; then
    check "unknown backend reports error and suggests a name" 1
else
    check "unknown backend reports error and suggests a name" 0
fi

"$ZC" transpile "$workdir/smoke.zc" --backend ast-dump -o "$workdir/smoke_ast" >/dev/null 2>&1
if [ -s "$workdir/smoke_ast" ]; then
    check "a known backend is selected by name" 1
else
    check "a known backend is selected by name" 0
fi

# --- Robustness: pathological inputs must exit cleanly (never crash) -------

# A crash (signal) yields exit code > 128; a clean compile or a normal
# diagnostic error stays <= 128. This guards the parser/codegen overflow
# fixes against regressions.
robust_check() {
    local name="$1"
    local code="$2"
    printf '%s\n' "$code" > "$workdir/robust.zc"
    "$ZC" build "$workdir/robust.zc" -o "$workdir/robust_out" >/dev/null 2>&1
    local rc=$?
    if [ "$rc" -le 128 ]; then
        check "$name" 1
    else
        check "$name" 0
    fi
}

robust_check "20-arg ?() scan does not crash" 'fn main() {
    let x = ?("prompt", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20);
    println "x";
}'

long_ident=$(python3 -c "print('A' * 1500)")
robust_check "very long qualified identifier does not crash" "fn main() {
    let x = Foo::$long_ident;
    println \"x\";
}"

robust_check "enum variant with 40 type args does not crash" "fn gen_enum() {
    return 0;
}
enum E { V(int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int) }
fn main() {
    let e = E::V(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40);
    println \"e\";
}"

robust_check "@derive on a struct with many long fields does not crash" "fn gen_derive() {
    return 0;
}
@derive(Eq)
struct S {
    field_0_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_2_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_3_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_4_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_5_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_6_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_7_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_8_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_9_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_10_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_11_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_12_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_13_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_14_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_15_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_16_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_17_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_18_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_19_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_20_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_21_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_22_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_23_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_24_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_25_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_26_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_27_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_28_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_29_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
}
fn main() {
    println \"ok\";
}"

garg=$(python3 -c "print('B' * 1500)")
robust_check "generic call with a very long type argument does not crash" "fn f<T>(x: T) -> T {
    return x;
}
fn main() {
    let y = f<$garg>(1);
    println \"y\";
}"

# --- Diagnostics: errors reported by Zen C, not by the C compiler ------------

# Builds a program that must fail with a Zen C error matching $3 (and never
# reach the C compiler).
diag_check() {
    local name="$1"
    local code="$2"
    local pattern="$3"
    printf '%s\n' "$code" > "$workdir/diag.zc"
    local out
    out=$("$ZC" build "$workdir/diag.zc" -o "$workdir/diag_out" 2>&1)
    if echo "$out" | grep -q "$pattern" && ! echo "$out" | grep -q "C compilation failed"; then
        check "$name" 1
    else
        check "$name" 0
    fi
}

diag_check "redefined function is a Zen C error" 'fn twice() -> int { return 1; }
fn twice() -> int { return 2; }
fn main() { println "{twice()}"; }' "Redefinition of function 'twice'"

diag_check "redefined method is a Zen C error" 'struct Counter { value: int; }
impl Counter { fn get(self) -> int { return 1; } }
impl Counter { fn get(self) -> int { return 2; } }
fn main() { let c = Counter { value: 0 }; println "{c.get()}"; }' "Redefinition of method 'Counter::get'"

# --- Imported files: diagnostics and #line name the imported file -----------

mkdir -p "$workdir/imported"
printf 'fn make() -> int {\n    let x: int = "text";\n    return 1;\n}\n' > "$workdir/imported/broken.zc"
printf 'import "imported/broken.zc"\nfn main() {\n    println "{make()}";\n}\n' > "$workdir/imported_main.zc"
out=$("$ZC" build "$workdir/imported_main.zc" -o "$workdir/imported_out" 2>&1)
if echo "$out" | grep -q "imported/broken.zc:2:" && ! echo "$out" | grep -q "imported_main.zc:"; then
    check "error in an imported file is reported at its own path" 1
else
    check "error in an imported file is reported at its own path" 0
fi

mkdir -p "$workdir/lib"
printf 'struct Counter { hits: int; }\nimpl Counter {\n    fn bump(self) { self.hits = self.hits + 1; }\n}\n' > "$workdir/lib/counter.zc"
printf 'import "lib/counter.zc"\nfn main() {\n    let c = Counter { hits: 0 };\n    c.bump();\n    println "{c.hits}";\n}\n' > "$workdir/lib_main.zc"
"$ZC" build --emit-c "$workdir/lib_main.zc" -o "$workdir/lib_out" >/dev/null 2>&1
line=$(awk '/#line/{l=$0} /^ZC_FUNC void Counter__bump\(Counter\* self\)$/{print l; exit}' "$workdir/lib_out.c" 2>/dev/null)
if echo "$line" | grep -q 'lib/counter.zc"'; then
    check "emit-c #line of an imported method names the imported file" 1
else
    check "emit-c #line of an imported method names the imported file" 0
fi

echo "----------------------------------------"
echo "Results (CLI):"
echo "-> Passed:  $PASSED"
echo "-> Failed:  $FAILED"
echo "----------------------------------------"

if [ "$FAILED" -ne 0 ]; then
    echo -e "Failed tests:$FAILED_TESTS"
    exit 1
fi
echo "All CLI tests passed!"
exit 0
