/*
 * Integration test: drive the engine through its public API like a real session,
 * then assert the fundamental invariant — across ALL accounts, postings sum to 0.
 */
#include "../src/account/account.h"
#include "../src/journal/journal.h"
#include "../src/store/store.h"
#include "../src/support/mb_test.h"

static mb_money global_posting_sum(mb_store *s) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(mb_store_handle(s), "SELECT COALESCE(SUM(amount),0) FROM posting;", -1, &st, NULL);
  sqlite3_step(st);
  mb_money v = (mb_money)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

TEST(engine, freelancer_session_balances) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));

  char bank[40], income[40], software[40];
  mb_account_new a = {.code="1000", .name="Business Checking", .type=MB_ACCT_ASSET,   .role=MB_ROLE_ACCOUNT};
  mb_account_new b = {.code="4000", .name="Consulting Income", .type=MB_ACCT_INCOME,  .role=MB_ROLE_CATEGORY};
  mb_account_new c = {.code="6010", .name="Software & Subs",   .type=MB_ACCT_EXPENSE, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &a, bank));
  ASSERT_OK(mb_account_create(s, &b, income));
  ASSERT_OK(mb_account_create(s, &c, software));

  /* got paid $2,500 */
  mb_posting_in pay[] = {{.account_id=bank, .amount=250000}, {.account_id=income, .amount=-250000}};
  char e1[40]; ASSERT_OK(mb_journal_post(s, "2026-06-01", "Client payment", MB_SRC_USER, pay, 2, e1));

  /* paid $60 for software from the bank */
  mb_posting_in exp[] = {{.account_id=software, .amount=6000}, {.account_id=bank, .amount=-6000}};
  char e2[40]; ASSERT_OK(mb_journal_post(s, "2026-06-03", "SaaS tool", MB_SRC_USER, exp, 2, e2));

  mb_money bal;
  ASSERT_OK(mb_account_balance(s, bank, &bal));     ASSERT_MONEY_EQ(bal, 244000);  /* 2500 - 60 */
  ASSERT_OK(mb_account_balance(s, income, &bal));   ASSERT_MONEY_EQ(bal, -250000);
  ASSERT_OK(mb_account_balance(s, software, &bal)); ASSERT_MONEY_EQ(bal, 6000);
  ASSERT_MONEY_EQ(global_posting_sum(s), 0);

  /* oops — reverse the software charge */
  char r2[40]; ASSERT_OK(mb_journal_reverse(s, e2, r2));
  ASSERT_OK(mb_account_balance(s, bank, &bal));     ASSERT_MONEY_EQ(bal, 250000);
  ASSERT_OK(mb_account_balance(s, software, &bal)); ASSERT_MONEY_EQ(bal, 0);
  ASSERT_MONEY_EQ(global_posting_sum(s), 0);  /* the books always balance */

  mb_store_close(s);
}
