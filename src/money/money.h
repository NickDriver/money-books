#ifndef MB_MONEY_H
#define MB_MONEY_H
/*
 * Money Books — money primitive (Phase 0, decision D12).
 *
 * All amounts are integer minor units (cents). Never floating point.
 * Every fallible op is MB_MUST_CHECK so a dropped error code won't compile.
 */
#include <stdint.h>
#include "../support/mb_error.h"

typedef int64_t mb_money;  /* signed cents */

/* a + b, with overflow detection */
mb_err mb_money_add(mb_money a, mb_money b, mb_money *out) MB_MUST_CHECK;

/* amount * qty, with overflow detection (for invoice lines) */
mb_err mb_money_mul(mb_money amount, int64_t qty, mb_money *out) MB_MUST_CHECK;

/* line total = round(unit_price_cents * qty_centi / 100), half away from zero.
 * qty_centi is quantity x100 (e.g. 1.5 units -> 150). */
mb_err mb_money_line_total(mb_money unit_price, int64_t qty_centi, mb_money *out) MB_MUST_CHECK;

/* parse a decimal string ("1234.56", "-12", "  9.9 ") into cents.
 * Rejects >2 fractional digits and any trailing junk. */
mb_err mb_money_parse(const char *s, mb_money *out) MB_MUST_CHECK;

/* format cents into buf as "[-]D.CC" */
mb_err mb_money_format(mb_money v, char *buf, size_t buflen) MB_MUST_CHECK;

#endif /* MB_MONEY_H */
