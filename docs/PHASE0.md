# Phase 0 — Testing, Error Handling & Build

> Foundation built *before* features, so testing and memory hygiene start at commit #1.
> Designed AI-first: failures are located, self-explaining, and machine-readable, because
> the agent is the primary reader of test/error output.

Status: **implemented & green.** `make test` builds under `-Werror` + ASan + UBSan and runs
unit + integration tests. A real first module (`money`, integer cents per D12) proves the tools.

---

## Layout

```
Makefile
src/
  support/
    mb_error.h / .c   # error model, invariants, logging, attributes
    mb_test.h         # auto-registering test harness (TEST + ASSERT_* macros)
  money/
    money.h / .c      # first real module; unit tests embedded under #ifdef MB_TEST
  test/
    mb_test.c         # the test runner (collect/sort/run/report)
    test_main.c       # main() -> mb_test_main()
tests/
  integration_smoke.c # integration tests (separate folder, Rust-style)
build/                # output (gitignored)
```

## Build modes (Makefile)

| Target | What it does |
|--------|--------------|
| `make` / `make test` | Debug build + **ASan + UBSan**, runs all tests. Default. |
| `make release` | `-O2 -flto -DNDEBUG` static lib, no sanitizers. `MB_INVARIANT` stays on. |
| `make leaks` | Runs tests under Apple's `leaks` (Valgrind/LSan are weak on arm64). |
| `make analyze` | Clang Static Analyzer over engine sources. |
| `make deadcode` | Coverage build → reports per-function execution; **0-hit = dead/untested**. |
| `make clean` | Remove `build/` and artifacts. |

Toolchain: **clang, C11**, `-Wall -Wextra -Wpedantic -Werror` + `-Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Wmissing-variable-declarations`. Conversion warnings are visible but
non-fatal (advisory). `-Wno-gnu-zero-variadic-macro-arguments` allows the ergonomic macros.

> **make gotcha:** deleting a source file does not relink (the binary is newer than what
> remains), so a stale test may "run." Run `make clean` after removing/renaming files.
> (We can add header/file dependency tracking later if it bites.)

---

## Testing — Rust-style, auto-registering

- **Unit tests live next to code**, in `#ifdef MB_TEST ... #endif`. The test build defines `-DMB_TEST`.
- **`TEST(suite, name) { ... }`** self-registers via `__attribute__((constructor))` — no manual list.
- **Integration tests** in `tests/` drive the public API across modules (will open in-memory SQLite
  and assert balances/reports once the engine lands).
- The body receives `mb_test *t` (used implicitly by the macros).

**Assertions** (`EXPECT_*` record & continue; `ASSERT_*` record & stop the test):
`ASSERT`, `EXPECT`, `ASSERT_TRUE/FALSE`, `ASSERT_EQ_INT`, `ASSERT_STR_EQ`,
`ASSERT_OK(expr)`, `ASSERT_ERR(expr, code)`, `ASSERT_MONEY_EQ(a, b)`, `FAILF(...)`.
They print `file:line` + **actual values** (money formatted as `15.00 != 16.00`), and `ASSERT_OK`
prints the failing error's name + last-error message.

**Runner flags:** `--filter <substr>` (run a subset), `--json` (one JSON object per test),
`--list` (list tests + locations). On a crash (signal), it names the test that was running.
On failure it prints a copy-paste re-run command per failing test. Exit code reflects pass/fail.

---

## Error handling — explicit, two-channel, compiler-enforced

**Channel 1 — expected/runtime failures → `mb_err` codes.** Single X-macro enum
(`MB_OK`, `MB_ERR_UNBALANCED`, `MB_ERR_OVERFLOW`, `MB_ERR_NOT_FOUND`, …). Functions return
`mb_err`; results via out-params. A thread-local **last-error** carries `{code, message, file,
line, func, trace}` for the UI/MCP layer.

- **`MB_FAIL(code [, "fmt", ...])`** — set + return a located, message-bearing error.
- **`MB_TRY(expr)`** — propagate like Rust's `?`, appending a breadcrumb to `trace` (so a deep
  failure shows its call path, not just the leaf).
- **`MB_MUST_CHECK`** (`warn_unused_result`) on every fallible function — **ignoring an error is a
  compile error.** Kills the #1 C accounting bug class (silently dropped error codes). Verified.

**Channel 2 — assertions:**
- **`MB_INVARIANT(cond [, msg])`** — data-integrity guard, **always on (even release)**. Aborts
  with a full diagnostic. Rationale: for accounting, *crash > corrupt books*.
- **`MB_DEBUG_ASSERT(cond [, msg])`** — ordinary bug-catching, compiled out under `NDEBUG`.

**Logging:** `MB_LOGD/I/W/E(...)` — leveled, located, to stderr.

---

## Dead-code strategy — three nets

1. **Compile-time:** `-Wunused*`, `-Wunreachable-code`, and `MB_MUST_CHECK` (ignored results).
2. **Coverage-as-dead-code (`make deadcode`):** runs the whole suite, reports functions executed
   **0 times** — each is either dead (remove) or untested (add a test). Strongest net.
3. **Static analysis (`make analyze`):** Clang Static Analyzer (dead stores, etc.).

> Note: `-Wunused-macros` is intentionally **not** in the default flags — it's too noisy across
> headers. Run it as an occasional manual sweep if hunting unused macros.

---

## Conventions going forward

- Every new module `foo.c` ships with embedded `#ifdef MB_TEST` unit tests in the same file.
- Every fallible function returns `mb_err` and is marked `MB_MUST_CHECK`; results via out-params.
- Validate inputs at the boundary → `MB_FAIL`; guard impossible states → `MB_INVARIANT`.
- Cross-module flows get an integration test in `tests/`.
- CI (later) runs `make test` (sanitized) + `make analyze`; periodically `make deadcode`.
