#ifndef MB_TEST_H
#define MB_TEST_H
/*
 * Money Books — minimal, auto-registering unit-test harness (Phase 0).
 *
 * Rust-style ergonomics in C:
 *   - Unit tests live next to the code, inside `#ifdef MB_TEST ... #endif`.
 *   - Each TEST(...) self-registers via a constructor — no manual list to keep.
 *   - One `test_runner` binary discovers & runs everything.
 *
 * AI-first: assertions print file:line + the ACTUAL values, the runner names the
 * crashing test on a signal, supports --filter / --json / --list, and prints a
 * copy-paste command to re-run any single failure.
 */
#include "mb_error.h"
#include <string.h>

typedef struct mb_test mb_test;
typedef void (*mb_test_fn)(mb_test *t);

struct mb_test {
  const char *suite;
  const char *name;
  const char *file;
  int         line;
  mb_test_fn  fn;
  int         failures;  /* set during a run */
  mb_test    *next;
};

void mb_test_register(mb_test *t);
void mb_test_fail(mb_test *t, const char *file, int line, const char *fmt, ...)
    MB_PRINTF(4, 5);
int  mb_test_main(int argc, char **argv);

/* Define a test. Body receives `mb_test *t` (used implicitly by the macros). */
#define TEST(suite_, name_) \
  static void mbt_fn_##suite_##_##name_(mb_test *t); \
  static mb_test mbt_node_##suite_##_##name_ = { \
      #suite_, #name_, __FILE__, __LINE__, mbt_fn_##suite_##_##name_, 0, NULL }; \
  __attribute__((constructor)) \
  static void mbt_ctor_##suite_##_##name_(void) { \
    mb_test_register(&mbt_node_##suite_##_##name_); } \
  static void mbt_fn_##suite_##_##name_(mb_test *t)

/* ---- assertions ----
 * EXPECT_* record a failure but continue; ASSERT_* record and stop the test. */
#define EXPECT(cond) \
  do { if (!(cond)) mb_test_fail(t, __FILE__, __LINE__, "EXPECT failed: %s", #cond); } while (0)

#define ASSERT(cond) \
  do { if (!(cond)) { mb_test_fail(t, __FILE__, __LINE__, "ASSERT failed: %s", #cond); return; } } while (0)

#define FAILF(...) \
  do { mb_test_fail(t, __FILE__, __LINE__, __VA_ARGS__); return; } while (0)

#define ASSERT_TRUE(a)  ASSERT(a)
#define ASSERT_FALSE(a) ASSERT(!(a))

#define ASSERT_EQ_INT(a, b) \
  do { long _a = (long)(a), _b = (long)(b); \
       if (_a != _b) { mb_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_EQ_INT(%s, %s): %ld != %ld", #a, #b, _a, _b); return; } } while (0)

#define ASSERT_STR_EQ(a, b) \
  do { const char *_a = (a), *_b = (b); \
       if (strcmp(_a ? _a : "", _b ? _b : "") != 0) { mb_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_STR_EQ(%s, %s): \"%s\" != \"%s\"", #a, #b, \
           _a ? _a : "(null)", _b ? _b : "(null)"); return; } } while (0)

/* expect MB_OK; on failure print the error name + last-error message */
#define ASSERT_OK(expr) \
  do { mb_err _e = (expr); \
       if (_e != MB_OK) { mb_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_OK(%s): got %s — %s", #expr, mb_err_name(_e), mb_last_error()->message); \
           return; } } while (0)

/* expect a specific error code */
#define ASSERT_ERR(expr, want) \
  do { mb_err _e = (expr); \
       if (_e != (want)) { mb_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_ERR(%s): expected %s, got %s", #expr, mb_err_name(want), mb_err_name(_e)); \
           return; } } while (0)

/* money is int64 cents — print as a currency-style diff */
#define ASSERT_MONEY_EQ(a, b) \
  do { long long _a = (long long)(a), _b = (long long)(b); \
       if (_a != _b) { mb_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_MONEY_EQ(%s, %s): %lld.%02lld != %lld.%02lld", #a, #b, \
           _a / 100, (_a < 0 ? -_a : _a) % 100, _b / 100, (_b < 0 ? -_b : _b) % 100); \
           return; } } while (0)

#endif /* MB_TEST_H */
