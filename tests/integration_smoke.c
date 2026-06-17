/*
 * Integration tests live in a separate folder (Rust-style) and exercise the public API across
 * modules, the way a real flow would: open an in-memory book, build documents, post, assert.
 */
#include "../src/store/store.h"
#include "../src/account/account.h"
#include "../src/counterparty/counterparty.h"
#include "../src/invoice/invoice.h"
#include "../src/journal/journal.h"
#include "../src/seed/seed.h"
#include "../src/money/money.h"
#include "../src/support/mb_test.h"

/* A real invoice built across store/account/counterparty/invoice/journal: three lines parsed from
 * dollar strings sum to the invoice total, and issuing it debits Accounts Receivable by that total.
 * (Was a money-only unit test mislabeled as integration — audit F9.) */
TEST(integration, invoice_total) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40];
  mb_account_new ai = {.code="4000", .name="Consulting Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &ai, income));
  char cp[40]; mb_counterparty_new c = {.name="Acme Co", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cp));

  char inv[40]; ASSERT_OK(mb_invoice_create(s, cp, "INV-1", NULL, NULL, inv));
  const char *prices[] = {"100.00", "49.99", "0.01"};   /* → $150.00 total */
  for (int i = 0; i < 3; i++) {
    mb_money price; ASSERT_OK(mb_money_parse(prices[i], &price));
    char lid[40];
    mb_invoice_line_in l = {.description = "Line", .qty_centi = 100, .unit_price = price, .account_id = income};
    ASSERT_OK(mb_invoice_add_line(s, inv, &l, lid));
  }

  mb_money total; ASSERT_OK(mb_invoice_total(s, inv, &total));
  ASSERT_MONEY_EQ(total, 15000);   /* $150.00 */
  char buf[32]; ASSERT_OK(mb_money_format(total, buf, sizeof buf));
  ASSERT_STR_EQ(buf, "150.00");

  /* issuing posts the accrual: Dr Accounts Receivable for the full total */
  ASSERT_OK(mb_invoice_issue(s, inv, "2026-08-01"));
  char ar[40]; ASSERT_OK(mb_store_meta_get(s, "ar_account_id", ar, sizeof ar));
  mb_money arbal; ASSERT_OK(mb_account_balance(s, ar, &arbal));
  ASSERT_MONEY_EQ(arbal, 15000);
  mb_store_close(s);
}
