# Zen C Test Guidelines

## Directory Structure

Almost every test is end-to-end: a `.zc` program compiled (and usually run) with
`zc`, or a runner that drives a `zc` binary from outside (`compiler/lsp/`,
`scripts/`). `unit/` is the exception: C tests that call the compiler's internal
functions directly.

```
tests/
├── bench/                 # Performance benchmarks (run manually)
├── compiler/
│   ├── analysis/          # Static analysis and type-checker tests
│   ├── codegen/           # Code generation verification
│   ├── diagnostics/       # Programs that must fail with a diagnostic
│   ├── diagnostics_output/ # Exact diagnostic output (see below)
│   ├── interop/           # C/C++/CUDA/ObjC interop
│   ├── lsp/               # Language server protocol tests
│   ├── parser/            # Parser-specific tests
│   └── typecheck/         # Type-checker integration tests
├── language/
│   ├── async/             # Async/await features
│   ├── basic/             # Basic syntax and semantics
│   ├── control-flow/      # If, match, loops, goto, defer, guard
│   ├── features/          # Language features by category:
│   │   ├── aliases/       #   Type aliases
│   │   ├── arrays-slices/ #   Arrays and slices
│   │   ├── asm/           #   Inline assembly
│   │   ├── closures/      #   Closures and lambdas
│   │   ├── collections/   #   Collection types
│   │   ├── compiler/      #   Compiler directives and attributes
│   │   ├── comptime/      #   Compile-time execution
│   │   ├── concurrency/   #   Concurrency primitives
│   │   ├── constants/     #   Constant declarations
│   │   ├── control_flow_ext/ # Extended control flow (defer, labeled break)
│   │   ├── destructuring/ #   Destructuring assignments
│   │   ├── embedding/     #   File embedding (embed)
│   │   ├── enums/         #   Enum types and codegen
│   │   ├── fstrings/      #   Formatted string literals
│   │   ├── functions/     #   Function-related features (default args, purity)
│   │   ├── iterators/     #   Iterator protocol
│   │   ├── keyword-ident/ #   Keywords used as identifiers
│   │   ├── match/         #   Pattern matching (advanced)
│   │   ├── memory/        #   Drop flags, move semantics, auto-free
│   │   ├── move/          #   Move checker flow-sensitive tests
│   │   ├── opaque-types/  #   Opaque structs and opaque aliases
│   │   ├── operators/     #   Operator overloading and sugar operators
│   │   ├── pointers/      #   Pointer type features
│   │   ├── safety/        #   Lifetime elision, escape analysis
│   │   ├── structs/       #   Struct features (const self, redefinition)
│   │   ├── traits/        #   Traits and trait implementations
│   │   ├── tuples/        #   Tuple types and operations
│   │   └── types/         #   Type system features (SIMD, portable, unicode)
│   ├── functions/         # Function declarations, attributes, fn pointers
│   ├── generics/          # Generic functions, structs, and traits
│   ├── keywords/          # Keyword-specific tests (typeof, volatile, etc.)
│   ├── misc/              # Miscellaneous integration tests
│   ├── modules/           # Module and import system
│   ├── string/            # String interpolation
│   └── types/             # Primitive types, type literals
├── misra/                 # MISRA compliance rules (156 files)
├── scripts/               # Test runner scripts and helpers
├── stdlib/                # Standard library tests
├── unit/                  # Unit tests in C of compiler internals (see below)
└── vendor/                # Third-party test code, unmodified (utest.h)
    ├── collections/       # Vec, slice, map iteration
    ├── networking/        # DNS, HTTP, WebSocket, URL
    ├── serialization/     # JSON, Base64
    ├── strings/           # String utilities, UTF-8, split
    └── system/            # Filesystem, environment, processes, threading
```

## Naming Conventions

| Pattern | Purpose | Example |
|---------|---------|---------|
| `test_<feature>.zc` | Main test for a feature | `test_if.zc` |
| `test_<feature>_<scenario>.zc` | Specific scenario | `test_async_concurrent.zc` |
| `_<prefix>_<name>.zc` | Helper module (skipped by runner) | `_test_cfg.zc` |
| `rule_<N>_<M>.zc` | MISRA rule | `rule_10_1.zc` |
| `rule_zen_<N>_<M>.zc` | Zen-specific rule | `rule_zen_2_2.zc` |
| `dir_<N>_<M>.zc` | MISRA directive | `dir_4_5.zc` |

Exceptions (non-standard naming but intentionally excluded from convention):
- `bench/test_*.zc`  --  performance benchmarks (no assertions, just timing)
- `compiler/codegen/test_*.zc`  --  compilation-only verification (no runtime assertions)

## Per-file Type Rules

| File type | Must have | Must NOT have |
|-----------|-----------|---------------|
| `test_*.zc` | Header comment, `test "..."` blocks, descriptive assertions | `fn main()` as entry point |
| `_*.zc` (helpers) | Header comment, `fn` definitions | `test "..."` blocks, `fn main()` |
| `// EXPECT: FAIL` | `// EXPECT: FAIL` on line 1, `fn main()` | `test "..."` blocks |
| Benchmarks | Comment describing what's measured | Assertions |

## Required Structure Per Test File

```zc
// <dir>/<feature>: one-line description of what this tests

// Arrange  --  set up inputs and expected values

// Act  --  execute the feature under test
let input = some_value;
let result = feature_under_test(input);

// Assert  --  verify with descriptive message
expect_eq(result, expected, "feature(x) should return y [when condition]");
```

### Default entry point rule

Every `test_*.zc` file MUST use `test "..." { }` blocks. The `fn main()` pattern is only allowed in `// EXPECT: FAIL` files (where the compiler must reject the whole file).

**Exception**: Files that are compilation-only smoke tests (no runtime assertions) may use `fn main()` if documented in a header comment.

## Assertion Messages

Every assertion MUST include a message that describes the expectation.

```zc
// BAD  --  unacceptable, will fail review:
assert(ok, "ok");
assert(result == 42);
assert(cond, "x == 42");  // tautological

// GOOD  --  required:
expect_eq(result, 42, "double(21) should return 42");
expect(result != null, "safe-nav should return null on missing field");
assert(x > 0, "Fibonacci(10) should be positive");
```

Message pattern: **`"<what> should <expected> [when <context>]"`**

## Per-test-block Structure

```
test "<scenario>" {
    // Arrange
    <setup inputs>

    // Act
    <run feature>

    // Assert
    expect_eq(actual, expected, "description");
}
```

Multiple `test "..."` blocks per file for related scenarios is allowed. Each block MUST be independent (no shared mutable state).

## What Every Feature Needs

| Aspect | Must test | Example |
|--------|-----------|---------|
| **Positive** | Feature works with typical valid input | `expect_eq(fn(42), 42, "fn(42) should return 42")` |
| **Negative** | Compiler rejects invalid input | `// EXPECT: FAIL` at top of separate file |
| **Zero/Empty** | Feature handles zero value or empty input | `expect_eq(fn(0), 0, "fn(0) should handle zero")` |
| **Boundary** | Feature handles max/min values | `expect_eq(fn(-2147483648), ...)` |
| **Null** | Feature handles null pointers safely | `expect(fn(null) == null, "fn(null) should return null")` |

### Negative Test Pattern

Files that test compiler rejection MUST start with `// EXPECT: FAIL`:

```zc
// EXPECT: FAIL
// Using a moved value should be rejected

struct Mover { val: int }
impl Drop for Mover { fn drop(self) {} }

fn consume(m: Mover) {}
fn main() {
    let m = Mover { val: 10 }
    consume(m)
    consume(m)  // Error: use of moved value
}
```

## File Header Comments

Every test file MUST start with a comment describing what it tests.

```zc
// control-flow/match: match expression compilation and runtime behavior
```

The format is: `// <dir/subdir>: <what this file tests>`

For negative tests (`// EXPECT: FAIL`), the header comes after the marker:

```zc
// EXPECT: FAIL
// keyword-ident/fn: 'fn' used as identifier should be rejected
```

## Import Conventions

- `std/test.zc` is auto-imported by the `test` keyword  --  do NOT import it explicitly
- Import other stdlib modules as needed:

```zc
import "std/core.zc"
import "std/string.zc"
import "std/result.zc"
```

## Running Tests

```sh
make test          # Run all language/compiler/stdlib tests (C mode)
make test-misra    # Run MISRA compliance suite
make test-lsp      # Run LSP protocol tests
make test-unit     # Run the C unit tests (also part of make test)

# Filter by name pattern:
ZC_TEST_FILTER="string" make test    # Only tests with "string" in the path
```

## Diagnostic Output Tests

`compiler/diagnostics_output/` checks what `zc` prints, not only that it fails. Each
`<name>.zc` starts with `// ARGS: <command> [options]` (e.g. `// ARGS: build --json`);
`tests/scripts/run_diagnostic_output_tests.sh` runs `zc` with them and compares the
output and exit code with `<name>.expected`. After an intended change to the
diagnostics, regenerate the expected files and review their diff:

```sh
UPDATE=1 tests/scripts/run_diagnostic_output_tests.sh
git diff tests/compiler/diagnostics_output/
```

## Unit Tests (C)

`unit/` tests internal functions of the compiler with
[utest.h](https://github.com/sheredom/utest.h), vendored unmodified in
`vendor/utest.h` (commit `706f357`, Unlicense, see `LICENSES/utest.txt`).

- One `test_<module>.c` per compiler module, e.g. `test_diagnostics.c`.
- A test is `UTEST(group, name) { ... }`; it registers itself, there is no list to
  update. `unit_main.c` only holds `UTEST_MAIN()`.
- `make test-unit` builds every `unit/*.c` with the compiler's objects (all but
  `main.o`) and the same warning flags, into `obj/tests/unit/run_unit`, and runs it.

## Adding a New Test

1. Choose the correct directory from the structure above
2. Write the test following the conventions
3. Run it: `./zc run tests/path/to/test.zc`
4. If it should fail at compile time, add `// EXPECT: FAIL` on line 1
5. Run the full suite: `make test`

## Code Review Checklist

- [ ] File starts with `// <dir>/<feature>: <description>` comment
- [ ] Uses `test "..." { }` blocks (unless `// EXPECT: FAIL`)
- [ ] Has positive case test
- [ ] Has edge case test (zero, empty, null, boundary)
- [ ] Has negative case if applicable (`// EXPECT: FAIL` file)
- [ ] Every assertion has a descriptive message (never bare `assert(cond)` or `"ok"`)
- [ ] Messages follow the pattern: `"<what> should <expected>"`
- [ ] Tests pass in isolation: `./zc run tests/path/to/test.zc`
- [ ] Full suite passes: `make test`
