#include "money.h"

#include <ctype.h>
#include <stdio.h>

mb_err mb_money_add(mb_money a, mb_money b, mb_money *out) {
  if (!out) return MB_FAIL(MB_ERR_INVALID_ARG, "out is NULL");
  if (__builtin_add_overflow(a, b, out))
    return MB_FAIL(MB_ERR_OVERFLOW, "%lld + %lld overflows",
                   (long long)a, (long long)b);
  return MB_OK;
}

mb_err mb_money_mul(mb_money amount, int64_t qty, mb_money *out) {
  if (!out) return MB_FAIL(MB_ERR_INVALID_ARG, "out is NULL");
  if (__builtin_mul_overflow(amount, qty, out))
    return MB_FAIL(MB_ERR_OVERFLOW, "%lld * %lld overflows",
                   (long long)amount, (long long)qty);
  return MB_OK;
}

mb_err mb_money_line_total(mb_money unit_price, int64_t qty_centi, mb_money *out) {
  if (!out) return MB_FAIL(MB_ERR_INVALID_ARG, "out is NULL");
  int64_t prod;
  if (__builtin_mul_overflow(unit_price, qty_centi, &prod))
    return MB_FAIL(MB_ERR_OVERFLOW, "%lld x %lld/100 overflows",
                   (long long)unit_price, (long long)qty_centi);
  int64_t q = prod / 100, r = prod % 100;   /* round half away from zero */
  if (r >= 50) q += 1;
  else if (r <= -50) q -= 1;
  *out = q;
  return MB_OK;
}

mb_err mb_money_parse(const char *s, mb_money *out) {
  if (!s || !out) return MB_FAIL(MB_ERR_INVALID_ARG, "null argument");

  while (isspace((unsigned char)*s)) s++;

  int neg = 0;
  if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }

  if (!isdigit((unsigned char)*s) && *s != '.')
    return MB_FAIL(MB_ERR_PARSE, "expected a digit");

  int64_t whole = 0;
  int saw_digit = 0;
  while (isdigit((unsigned char)*s)) {
    if (__builtin_mul_overflow(whole, (int64_t)10, &whole) ||
        __builtin_add_overflow(whole, (int64_t)(*s - '0'), &whole))
      return MB_FAIL(MB_ERR_OVERFLOW, "value too large");
    s++;
    saw_digit = 1;
  }

  int64_t cents = 0;
  if (*s == '.') {
    s++;
    for (int d = 0; d < 2; d++) {
      if (isdigit((unsigned char)*s)) { cents = cents * 10 + (*s - '0'); s++; saw_digit = 1; }
      else { cents = cents * 10; }  /* pad missing fractional digit */
    }
    if (isdigit((unsigned char)*s))
      return MB_FAIL(MB_ERR_PARSE, "more than 2 decimal places");
  }

  while (isspace((unsigned char)*s)) s++;
  if (*s != '\0') return MB_FAIL(MB_ERR_PARSE, "trailing junk: \"%s\"", s);
  if (!saw_digit) return MB_FAIL(MB_ERR_PARSE, "no digits");

  int64_t total;
  if (__builtin_mul_overflow(whole, (int64_t)100, &total) ||
      __builtin_add_overflow(total, cents, &total))
    return MB_FAIL(MB_ERR_OVERFLOW, "value too large");

  *out = neg ? -total : total;
  return MB_OK;
}

mb_err mb_money_format(mb_money v, char *buf, size_t buflen) {
  if (!buf) return MB_FAIL(MB_ERR_INVALID_ARG, "buf is NULL");
  /* avoid UB negating INT64_MIN: work in unsigned magnitude */
  int neg = v < 0;
  uint64_t mag = neg ? (~(uint64_t)v + 1u) : (uint64_t)v;
  int n = snprintf(buf, buflen, "%s%llu.%02llu", neg ? "-" : "",
                   (unsigned long long)(mag / 100),
                   (unsigned long long)(mag % 100));
  if (n < 0 || (size_t)n >= buflen)
    return MB_FAIL(MB_ERR_INVALID_ARG, "buffer too small");
  return MB_OK;
}

/* ---------------------------------------------------------------------------
 * Unit tests live next to the code (Rust-style). Compiled only with -DMB_TEST.
 * ------------------------------------------------------------------------- */
#ifdef MB_TEST
#include "../support/mb_test.h"
#include <limits.h>

TEST(money, parse_basic) {
  mb_money m;
  ASSERT_OK(mb_money_parse("12.34", &m));  ASSERT_MONEY_EQ(m, 1234);
  ASSERT_OK(mb_money_parse("0", &m));       ASSERT_MONEY_EQ(m, 0);
  ASSERT_OK(mb_money_parse("-5.6", &m));    ASSERT_MONEY_EQ(m, -560);
  ASSERT_OK(mb_money_parse("  7 ", &m));    ASSERT_MONEY_EQ(m, 700);
  ASSERT_OK(mb_money_parse(".5", &m));      ASSERT_MONEY_EQ(m, 50);
}

TEST(money, parse_rejects) {
  mb_money m;
  ASSERT_ERR(mb_money_parse("1.234", &m), MB_ERR_PARSE);
  ASSERT_ERR(mb_money_parse("abc", &m),   MB_ERR_PARSE);
  ASSERT_ERR(mb_money_parse("1.2.3", &m), MB_ERR_PARSE);
  ASSERT_ERR(mb_money_parse("", &m),      MB_ERR_PARSE);
}

TEST(money, add_detects_overflow) {
  mb_money m;
  ASSERT_ERR(mb_money_add(INT64_MAX, 1, &m), MB_ERR_OVERFLOW);
  ASSERT_OK(mb_money_add(100, 200, &m));  ASSERT_MONEY_EQ(m, 300);
}

TEST(money, mul_detects_overflow) {
  mb_money m;
  ASSERT_ERR(mb_money_mul(INT64_MAX, 2, &m), MB_ERR_OVERFLOW);
  ASSERT_OK(mb_money_mul(1500, 3, &m));  ASSERT_MONEY_EQ(m, 4500);
}

TEST(money, line_total_rounds) {
  mb_money m;
  ASSERT_OK(mb_money_line_total(15000, 150, &m)); ASSERT_MONEY_EQ(m, 22500);  /* 150.00 x 1.5 */
  ASSERT_OK(mb_money_line_total(10000, 100, &m)); ASSERT_MONEY_EQ(m, 10000);  /* 100.00 x 1   */
  ASSERT_OK(mb_money_line_total(333, 150, &m));   ASSERT_MONEY_EQ(m, 500);    /* 3.33 x 1.5 = 4.995 -> 5.00 */
  ASSERT_ERR(mb_money_line_total(INT64_MAX, 200, &m), MB_ERR_OVERFLOW);
}

TEST(money, format_roundtrip) {
  char buf[32];
  mb_money m;
  ASSERT_OK(mb_money_format(-1234, buf, sizeof buf));  ASSERT_STR_EQ(buf, "-12.34");
  ASSERT_OK(mb_money_parse(buf, &m));                  ASSERT_MONEY_EQ(m, -1234);
  ASSERT_OK(mb_money_format(5, buf, sizeof buf));      ASSERT_STR_EQ(buf, "0.05");
}
#endif /* MB_TEST */
