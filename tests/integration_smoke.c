/*
 * Integration tests live in a separate folder (Rust-style) and exercise the
 * public API across modules, the way a real flow would. For now this composes
 * the money primitive; as the engine grows these will open an in-memory SQLite
 * book, post entries, and assert balances/reports.
 */
#include "../src/money/money.h"
#include "../src/support/mb_test.h"

TEST(integration, invoice_total) {
  const char *lines[] = {"100.00", "49.99", "0.01"};
  mb_money total = 0;
  for (int i = 0; i < 3; i++) {
    mb_money m;
    ASSERT_OK(mb_money_parse(lines[i], &m));
    ASSERT_OK(mb_money_add(total, m, &total));
  }
  ASSERT_MONEY_EQ(total, 15000);  /* $150.00 */

  char buf[32];
  ASSERT_OK(mb_money_format(total, buf, sizeof buf));
  ASSERT_STR_EQ(buf, "150.00");
}
