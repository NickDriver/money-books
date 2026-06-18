#include "report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mb_account_type parse_type(const char *s) {
  if (!strcmp(s, "LIABILITY")) return MB_ACCT_LIABILITY;
  if (!strcmp(s, "EQUITY"))    return MB_ACCT_EQUITY;
  if (!strcmp(s, "INCOME"))    return MB_ACCT_INCOME;
  if (!strcmp(s, "EXPENSE"))   return MB_ACCT_EXPENSE;
  return MB_ACCT_ASSET;
}

mb_err mb_report_balances(mb_store *s, const char *from, const char *to,
                          mb_acct_balance **rows, int *n) {
  /* Date filter must be inside SUM(CASE ...): a LEFT JOIN condition would only null the
   * entry, not exclude the posting. CASE keeps zero-posting accounts present (balance 0). */
  static const char *SQL =
    "SELECT a.id, a.code, a.name, a.type, "
    "  COALESCE(SUM(CASE WHEN (?1 IS NULL OR e.date >= ?1) "
    "                     AND (?2 IS NULL OR e.date <= ?2) THEN p.amount ELSE 0 END), 0) "
    "FROM account a "
    "LEFT JOIN posting p ON p.account_id = a.id "
    "LEFT JOIN journal_entry e ON e.id = p.entry_id "
    "GROUP BY a.id ORDER BY a.code, a.name;";
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), SQL, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  if (from) sqlite3_bind_text(st, 1, from, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 1);
  if (to)   sqlite3_bind_text(st, 2, to, -1, SQLITE_TRANSIENT);   else sqlite3_bind_null(st, 2);

  int cap = 32, cnt = 0;
  mb_acct_balance *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (cnt == cap) {
      cap *= 2;
      mb_acct_balance *na = realloc(arr, (size_t)cap * sizeof *na);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    mb_acct_balance *r = &arr[cnt++];
    memset(r, 0, sizeof *r);
    const char *c;
    snprintf(r->account_id, sizeof r->account_id, "%s", (const char *)sqlite3_column_text(st, 0));
    c = (const char *)sqlite3_column_text(st, 1); snprintf(r->code, sizeof r->code, "%s", c ? c : "");
    snprintf(r->name, sizeof r->name, "%s", (const char *)sqlite3_column_text(st, 2));
    r->type = parse_type((const char *)sqlite3_column_text(st, 3));
    r->balance = (mb_money)sqlite3_column_int64(st, 4);
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) { free(arr); return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  *rows = arr;
  *n = cnt;
  return MB_OK;
}

mb_err mb_report_trial_balance(mb_store *s, const char *as_of,
                               mb_acct_balance **rows, int *n, mb_trial_balance *summary) {
  MB_TRY(mb_report_balances(s, NULL, as_of, rows, n));
  summary->total_debit = 0;
  summary->total_credit = 0;
  for (int i = 0; i < *n; i++) {
    mb_money b = (*rows)[i].balance;
    if (b > 0) summary->total_debit += b;
    else if (b < 0) summary->total_credit += -b;
  }
  summary->balanced = (summary->total_debit == summary->total_credit);
  return MB_OK;
}

mb_err mb_report_pnl(mb_store *s, const char *from, const char *to, mb_pnl *out) {
  mb_acct_balance *rows = NULL;
  int n = 0;
  MB_TRY(mb_report_balances(s, from, to, &rows, &n));
  out->income = 0;
  out->expense = 0;
  for (int i = 0; i < n; i++) {
    if (rows[i].type == MB_ACCT_INCOME)  out->income  += -rows[i].balance;  /* credit-normal */
    if (rows[i].type == MB_ACCT_EXPENSE) out->expense +=  rows[i].balance;  /* debit-normal */
  }
  out->net = out->income - out->expense;
  free(rows);
  return MB_OK;
}

mb_err mb_report_balance_sheet(mb_store *s, const char *as_of, mb_balance_sheet *out) {
  mb_acct_balance *rows = NULL;
  int n = 0;
  MB_TRY(mb_report_balances(s, NULL, as_of, &rows, &n));
  mb_money income = 0, expense = 0;
  out->assets = 0;
  out->liabilities = 0;
  out->equity = 0;
  for (int i = 0; i < n; i++) {
    mb_money b = rows[i].balance;
    switch (rows[i].type) {
      case MB_ACCT_ASSET:     out->assets      += b;  break;
      case MB_ACCT_LIABILITY: out->liabilities += -b; break;  /* credit-normal */
      case MB_ACCT_EQUITY:    out->equity      += -b; break;  /* credit-normal */
      case MB_ACCT_INCOME:    income  += -b; break;
      case MB_ACCT_EXPENSE:   expense +=  b; break;
    }
  }
  out->net_income = income - expense;
  out->balanced = (out->assets == out->liabilities + out->equity + out->net_income);
  free(rows);
  return MB_OK;
}

mb_err mb_report_ledger(mb_store *s, const char *account_id, const char *from, const char *to,
                        mb_ledger_row **rows, int *n) {
  mb_money opening = 0;
  if (from) {
    sqlite3_stmt *o;
    if (sqlite3_prepare_v2(mb_store_handle(s),
          "SELECT COALESCE(SUM(p.amount),0) FROM posting p JOIN journal_entry e ON e.id=p.entry_id "
          "WHERE p.account_id=? AND e.date < ?;", -1, &o, NULL) != SQLITE_OK)
      return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
    sqlite3_bind_text(o, 1, account_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(o, 2, from, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(o) == SQLITE_ROW) opening = (mb_money)sqlite3_column_int64(o, 0);
    sqlite3_finalize(o);
  }

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT e.id, e.date, e.memo, p.amount "
        "FROM posting p JOIN journal_entry e ON e.id=p.entry_id "
        "WHERE p.account_id=?1 AND (?2 IS NULL OR e.date>=?2) AND (?3 IS NULL OR e.date<=?3) "
        "ORDER BY e.date, e.seq;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, account_id, -1, SQLITE_TRANSIENT);
  if (from) sqlite3_bind_text(st, 2, from, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 2);
  if (to)   sqlite3_bind_text(st, 3, to, -1, SQLITE_TRANSIENT);   else sqlite3_bind_null(st, 3);

  int cap = 32, cnt = 0;
  mb_ledger_row *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  mb_money running = opening;
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (cnt == cap) {
      cap *= 2;
      mb_ledger_row *na = realloc(arr, (size_t)cap * sizeof *na);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    mb_ledger_row *r = &arr[cnt++];
    memset(r, 0, sizeof *r);
    const char *c;
    snprintf(r->entry_id, sizeof r->entry_id, "%s", (const char *)sqlite3_column_text(st, 0));
    snprintf(r->date, sizeof r->date, "%s", (const char *)sqlite3_column_text(st, 1));
    c = (const char *)sqlite3_column_text(st, 2); snprintf(r->memo, sizeof r->memo, "%s", c ? c : "");
    r->amount = (mb_money)sqlite3_column_int64(st, 3);
    running += r->amount;
    r->running = running;
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) { free(arr); return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  *rows = arr;
  *n = cnt;
  return MB_OK;
}

/* shared aging for invoices ('INVOICE'/invoice/invoice_line/invoice_id) and bills */
static mb_err aging(mb_store *s, const char *as_of, const char *kind, const char *table,
                    const char *line_table, const char *fk, mb_aging *out) {
  char sql[640];
  snprintf(sql, sizeof sql,
    "SELECT "
    "  (SELECT COALESCE(SUM(line_total),0) FROM %s WHERE %s=t.id) "
    "  - (SELECT COALESCE(SUM(amount),0) FROM allocation WHERE target_kind='%s' AND target_id=t.id) AS outstanding, "
    "  CAST(julianday(COALESCE(?1, date('now'))) "
    "       - julianday(COALESCE(t.due_date, t.issue_date, date('now'))) AS INTEGER) AS age "
    "FROM %s t WHERE t.status IN ('OPEN','PARTIAL');",
    line_table, fk, kind, table);

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), sql, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  if (as_of) sqlite3_bind_text(st, 1, as_of, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 1);

  memset(out, 0, sizeof *out);
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    mb_money outstanding = (mb_money)sqlite3_column_int64(st, 0);
    long age = (long)sqlite3_column_int64(st, 1);
    if (outstanding == 0) continue;
    if (age <= 0)        out->current  += outstanding;
    else if (age <= 30)  out->d1_30    += outstanding;
    else if (age <= 60)  out->d31_60   += outstanding;
    else if (age <= 90)  out->d61_90   += outstanding;
    else                 out->d90_plus += outstanding;
    out->total += outstanding;
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  return MB_OK;
}

mb_err mb_report_ar_aging(mb_store *s, const char *as_of, mb_aging *out) {
  return aging(s, as_of, "INVOICE", "invoice", "invoice_line", "invoice_id", out);
}
mb_err mb_report_ap_aging(mb_store *s, const char *as_of, mb_aging *out) {
  return aging(s, as_of, "BILL", "bill", "bill_line", "bill_id", out);
}

mb_err mb_report_cash_flow(mb_store *s, const char *from, const char *to, mb_cash_flow *out) {
  static const char *SQL =
    "SELECT "
    "  COALESCE(SUM(CASE WHEN p.amount>0 AND (?1 IS NULL OR e.date>=?1) AND (?2 IS NULL OR e.date<=?2) "
    "                    THEN p.amount ELSE 0 END),0), "
    "  COALESCE(SUM(CASE WHEN p.amount<0 AND (?1 IS NULL OR e.date>=?1) AND (?2 IS NULL OR e.date<=?2) "
    "                    THEN -p.amount ELSE 0 END),0) "
    "FROM posting p JOIN journal_entry e ON e.id=p.entry_id JOIN account a ON a.id=p.account_id "
    "WHERE a.type='ASSET' AND a.role='ACCOUNT';";
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), SQL, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  if (from) sqlite3_bind_text(st, 1, from, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 1);
  if (to)   sqlite3_bind_text(st, 2, to, -1, SQLITE_TRANSIENT);   else sqlite3_bind_null(st, 2);
  mb_err e;
  if (sqlite3_step(st) == SQLITE_ROW) {
    out->inflow = (mb_money)sqlite3_column_int64(st, 0);
    out->outflow = (mb_money)sqlite3_column_int64(st, 1);
    out->net = out->inflow - out->outflow;
    e = MB_OK;
  } else {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  }
  sqlite3_finalize(st);
  return e;
}

mb_err mb_report_journal(mb_store *s, const char *from, const char *to,
                         mb_journal_row **rows, int *n) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT e.id, e.date, COALESCE(e.memo,''), e.source, e.status, "
        "  (SELECT COALESCE(SUM(CASE WHEN p.amount>0 THEN p.amount ELSE 0 END),0) "
        "   FROM posting p WHERE p.entry_id=e.id), "
        "  CASE "
        "    WHEN EXISTS(SELECT 1 FROM posting p JOIN account a ON a.id=p.account_id "
        "                WHERE p.entry_id=e.id AND a.type='INCOME') THEN 'INCOME' "
        "    WHEN EXISTS(SELECT 1 FROM posting p JOIN account a ON a.id=p.account_id "
        "                WHERE p.entry_id=e.id AND a.type='EXPENSE') THEN 'EXPENSE' "
        "    ELSE 'OTHER' END "
        "FROM journal_entry e "
        "WHERE (?1 IS NULL OR e.date>=?1) AND (?2 IS NULL OR e.date<=?2) "
        "ORDER BY e.date DESC, e.seq DESC;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  if (from) sqlite3_bind_text(st, 1, from, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 1);
  if (to)   sqlite3_bind_text(st, 2, to, -1, SQLITE_TRANSIENT);   else sqlite3_bind_null(st, 2);
  int cap = 32, cnt = 0;
  mb_journal_row *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (cnt == cap) {
      cap *= 2;
      mb_journal_row *na = realloc(arr, (size_t)cap * sizeof *na);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    mb_journal_row *r = &arr[cnt++];
    memset(r, 0, sizeof *r);
    snprintf(r->entry_id, sizeof r->entry_id, "%s", (const char *)sqlite3_column_text(st, 0));
    snprintf(r->date, sizeof r->date, "%s", (const char *)sqlite3_column_text(st, 1));
    snprintf(r->memo, sizeof r->memo, "%s", (const char *)sqlite3_column_text(st, 2));
    snprintf(r->source, sizeof r->source, "%s", (const char *)sqlite3_column_text(st, 3));
    snprintf(r->status, sizeof r->status, "%s", (const char *)sqlite3_column_text(st, 4));
    r->amount = (mb_money)sqlite3_column_int64(st, 5);
    snprintf(r->flow, sizeof r->flow, "%s", (const char *)sqlite3_column_text(st, 6));
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) { free(arr); return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  *rows = arr;
  *n = cnt;
  return MB_OK;
}

mb_err mb_report_category_txns(mb_store *s, const char *acct_type, const char *from, const char *to,
                               mb_cat_txn_row **rows, int *n) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT e.id, e.date, COALESCE(e.memo,''), a.id, a.name, ABS(p.amount) "
        "FROM posting p "
        "JOIN journal_entry e ON e.id=p.entry_id "
        "JOIN account a ON a.id=p.account_id "
        "WHERE a.type=?1 AND e.status='POSTED' "
        "  AND (?2 IS NULL OR e.date>=?2) AND (?3 IS NULL OR e.date<=?3) "
        "ORDER BY e.date DESC, e.seq DESC;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, acct_type, -1, SQLITE_TRANSIENT);
  if (from) sqlite3_bind_text(st, 2, from, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 2);
  if (to)   sqlite3_bind_text(st, 3, to, -1, SQLITE_TRANSIENT);   else sqlite3_bind_null(st, 3);
  int cap = 32, cnt = 0;
  mb_cat_txn_row *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (cnt == cap) {
      cap *= 2;
      mb_cat_txn_row *na = realloc(arr, (size_t)cap * sizeof *na);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    mb_cat_txn_row *r = &arr[cnt++];
    memset(r, 0, sizeof *r);
    snprintf(r->entry_id, sizeof r->entry_id, "%s", (const char *)sqlite3_column_text(st, 0));
    snprintf(r->date, sizeof r->date, "%s", (const char *)sqlite3_column_text(st, 1));
    snprintf(r->memo, sizeof r->memo, "%s", (const char *)sqlite3_column_text(st, 2));
    snprintf(r->category_id, sizeof r->category_id, "%s", (const char *)sqlite3_column_text(st, 3));
    snprintf(r->category_name, sizeof r->category_name, "%s", (const char *)sqlite3_column_text(st, 4));
    r->amount = (mb_money)sqlite3_column_int64(st, 5);
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) { free(arr); return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  *rows = arr;
  *n = cnt;
  return MB_OK;
}

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../journal/journal.h"
#include "../counterparty/counterparty.h"
#include "../invoice/invoice.h"
#include "../bill/bill.h"
#include "../seed/seed.h"

/* helper: post a simple two-line entry */
static mb_err post2(mb_store *s, const char *date, const char *dr, const char *cr, mb_money amt) {
  mb_posting_in p[] = {{.account_id = dr, .amount = amt}, {.account_id = cr, .amount = -amt}};
  char e[40];
  return mb_journal_post(s, date, "t", MB_SRC_USER, p, 2, e);
}

static void seed_scenario(mb_store *s, char bank[40], char income[40], char expense[40]) {
  mb_account_new b = {.code="1000", .name="Checking", .type=MB_ACCT_ASSET,   .role=MB_ROLE_ACCOUNT};
  mb_account_new i = {.code="4000", .name="Income",   .type=MB_ACCT_INCOME,  .role=MB_ROLE_CATEGORY};
  mb_account_new x = {.code="6000", .name="Expense",  .type=MB_ACCT_EXPENSE, .role=MB_ROLE_CATEGORY};
  (void)mb_account_create(s, &b, bank);
  (void)mb_account_create(s, &i, income);
  (void)mb_account_create(s, &x, expense);
}

TEST(report, trial_balance_balances) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_scenario(s, bank, income, expense);
  ASSERT_OK(post2(s, "2026-01-10", bank, income, 100000));  /* revenue 1000 */
  ASSERT_OK(post2(s, "2026-01-20", expense, bank, 30000));  /* expense 300 */

  mb_acct_balance *rows = NULL; int n = 0; mb_trial_balance tb;
  ASSERT_OK(mb_report_trial_balance(s, NULL, &rows, &n, &tb));
  ASSERT_MONEY_EQ(tb.total_debit, 100000);   /* bank 700 + expense 300 */
  ASSERT_MONEY_EQ(tb.total_credit, 100000);  /* income 1000 */
  ASSERT_EQ_INT(tb.balanced, 1);
  free(rows);
  mb_store_close(s);
}

TEST(report, pnl_and_balance_sheet) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_scenario(s, bank, income, expense);
  ASSERT_OK(post2(s, "2026-01-10", bank, income, 100000));
  ASSERT_OK(post2(s, "2026-01-20", expense, bank, 30000));

  mb_pnl pl;
  ASSERT_OK(mb_report_pnl(s, NULL, NULL, &pl));
  ASSERT_MONEY_EQ(pl.income, 100000);
  ASSERT_MONEY_EQ(pl.expense, 30000);
  ASSERT_MONEY_EQ(pl.net, 70000);

  mb_balance_sheet bs;
  ASSERT_OK(mb_report_balance_sheet(s, NULL, &bs));
  ASSERT_MONEY_EQ(bs.assets, 70000);       /* bank */
  ASSERT_MONEY_EQ(bs.liabilities, 0);
  ASSERT_MONEY_EQ(bs.equity, 0);
  ASSERT_MONEY_EQ(bs.net_income, 70000);
  ASSERT_EQ_INT(bs.balanced, 1);           /* 70000 == 0 + 0 + 70000 */
  mb_store_close(s);
}

TEST(report, pnl_date_window) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_scenario(s, bank, income, expense);
  ASSERT_OK(post2(s, "2026-01-10", bank, income, 50000));
  ASSERT_OK(post2(s, "2026-02-10", bank, income, 70000));
  mb_pnl q1;
  ASSERT_OK(mb_report_pnl(s, "2026-01-01", "2026-01-31", &q1));
  ASSERT_MONEY_EQ(q1.income, 50000);   /* only January */
  mb_pnl all;
  ASSERT_OK(mb_report_pnl(s, NULL, NULL, &all));
  ASSERT_MONEY_EQ(all.income, 120000);
  mb_store_close(s);
}

TEST(report, ledger_running_balance) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_scenario(s, bank, income, expense);
  ASSERT_OK(post2(s, "2026-01-05", bank, income, 100000));  /* +1000 */
  ASSERT_OK(post2(s, "2026-01-15", expense, bank, 30000));  /* -300  */
  ASSERT_OK(post2(s, "2026-01-25", bank, income, 50000));   /* +500  */

  mb_ledger_row *rows = NULL; int n = 0;
  ASSERT_OK(mb_report_ledger(s, bank, NULL, NULL, &rows, &n));
  ASSERT_EQ_INT(n, 3);
  ASSERT_MONEY_EQ(rows[0].running, 100000);
  ASSERT_MONEY_EQ(rows[1].running, 70000);
  ASSERT_MONEY_EQ(rows[2].running, 120000);
  free(rows);

  /* windowed: opening balance seeds the running total */
  ASSERT_OK(mb_report_ledger(s, bank, "2026-01-20", NULL, &rows, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_MONEY_EQ(rows[0].running, 120000);  /* opening 70000 + 50000 */
  free(rows);
  mb_store_close(s);
}

TEST(report, ar_aging_buckets) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40];
  mb_account_new ai = {.code="4000", .name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &ai, income));
  char cp[40]; mb_counterparty_new c = {.name="Acme", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cp));

  char inv[40]; ASSERT_OK(mb_invoice_create(s, cp, "INV-1", "2026-01-31", NULL, inv));
  char lid[40];
  mb_invoice_line_in l = {.description="Work", .qty_centi=100, .unit_price=100000, .account_id=income};
  ASSERT_OK(mb_invoice_add_line(s, inv, &l, lid));
  ASSERT_OK(mb_invoice_issue(s, inv, "2026-01-01"));

  /* as of Feb 10 -> ~10 days past the Jan 31 due date -> 1-30 bucket */
  mb_aging ag;
  ASSERT_OK(mb_report_ar_aging(s, "2026-02-10", &ag));
  ASSERT_MONEY_EQ(ag.total, 100000);
  ASSERT_MONEY_EQ(ag.d1_30, 100000);
  ASSERT_MONEY_EQ(ag.current, 0);
  mb_store_close(s);
}

/* helpers for the all-buckets aging test: one issued invoice / entered bill with a chosen due date */
static void aged_invoice(mb_test *t, mb_store *s, const char *cp, const char *income,
                         const char *number, const char *due, mb_money cents) {
  char inv[40]; ASSERT_OK(mb_invoice_create(s, cp, number, due, NULL, inv));
  char lid[40];
  mb_invoice_line_in l = {.description="Work", .qty_centi=100, .unit_price=cents, .account_id=income};
  ASSERT_OK(mb_invoice_add_line(s, inv, &l, lid));
  ASSERT_OK(mb_invoice_issue(s, inv, due));
}
static void aged_bill(mb_test *t, mb_store *s, const char *vendor, const char *expense,
                      const char *number, const char *due, mb_money cents) {
  char bill[40]; ASSERT_OK(mb_bill_create(s, vendor, number, due, NULL, bill));
  char lid[40];
  mb_bill_line_in l = {.description="Parts", .qty_centi=100, .unit_price=cents, .account_id=expense};
  ASSERT_OK(mb_bill_add_line(s, bill, &l, lid));
  ASSERT_OK(mb_bill_enter(s, bill, due));
}

/* audit F6: every aging bucket lands correctly given due dates, on BOTH AR and AP.
 * As of 2026-06-30, ages from due date: +future=current, 10d=1-30, 41d=31-60, 71d=61-90, 180d=90+. */
TEST(report, aging_all_buckets_ar_and_ap) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40], expense[40];
  mb_account_new ai = {.code="4000", .name="Income",  .type=MB_ACCT_INCOME,  .role=MB_ROLE_CATEGORY};
  mb_account_new ae = {.code="6000", .name="Parts",   .type=MB_ACCT_EXPENSE, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &ai, income));
  ASSERT_OK(mb_account_create(s, &ae, expense));
  char cust[40]; mb_counterparty_new c = {.name="Acme", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cust));
  char vend[40]; mb_counterparty_new v = {.name="Supplier", .kind=MB_CP_VENDOR};
  ASSERT_OK(mb_counterparty_create(s, &v, vend));
  const char *as_of = "2026-06-30";

  /* AR: one invoice per bucket */
  aged_invoice(t, s, cust, income, "C", "2026-07-15", 10000);  /* future  → current */
  aged_invoice(t, s, cust, income, "A", "2026-06-20", 20000);  /* 10 days → 1-30   */
  aged_invoice(t, s, cust, income, "B", "2026-05-20", 30000);  /* 41 days → 31-60  */
  aged_invoice(t, s, cust, income, "D", "2026-04-20", 40000);  /* 71 days → 61-90  */
  aged_invoice(t, s, cust, income, "E", "2026-01-01", 50000);  /* 180days → 90+    */

  mb_aging ar;
  ASSERT_OK(mb_report_ar_aging(s, as_of, &ar));
  ASSERT_MONEY_EQ(ar.current,  10000);
  ASSERT_MONEY_EQ(ar.d1_30,    20000);
  ASSERT_MONEY_EQ(ar.d31_60,   30000);
  ASSERT_MONEY_EQ(ar.d61_90,   40000);
  ASSERT_MONEY_EQ(ar.d90_plus, 50000);
  ASSERT_MONEY_EQ(ar.total,   150000);

  /* AP: two bills in distinct buckets (mb_report_ap_aging had no test at all) */
  aged_bill(t, s, vend, expense, "P1", "2026-06-25", 5000);    /* 5 days  → 1-30  */
  aged_bill(t, s, vend, expense, "P2", "2026-03-01", 7000);    /* ~121d   → 90+   */
  mb_aging ap;
  ASSERT_OK(mb_report_ap_aging(s, as_of, &ap));
  ASSERT_MONEY_EQ(ap.d1_30,    5000);
  ASSERT_MONEY_EQ(ap.d90_plus, 7000);
  ASSERT_MONEY_EQ(ap.total,   12000);
  ASSERT_MONEY_EQ(ap.current,  0);
  mb_store_close(s);
}

TEST(report, cash_flow_in_out) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_scenario(s, bank, income, expense);
  ASSERT_OK(post2(s, "2026-03-01", bank, income, 100000));  /* cash in 1000 */
  ASSERT_OK(post2(s, "2026-03-05", expense, bank, 30000));  /* cash out 300 */
  mb_cash_flow cf;
  ASSERT_OK(mb_report_cash_flow(s, NULL, NULL, &cf));
  ASSERT_MONEY_EQ(cf.inflow, 100000);
  ASSERT_MONEY_EQ(cf.outflow, 30000);
  ASSERT_MONEY_EQ(cf.net, 70000);
  mb_store_close(s);
}

TEST(report, journal_lists_all_entries) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_scenario(s, bank, income, expense);
  ASSERT_OK(post2(s, "2026-01-10", bank, income, 100000));
  ASSERT_OK(post2(s, "2026-01-20", expense, bank, 30000));

  mb_journal_row *rows = NULL; int n = 0;
  ASSERT_OK(mb_report_journal(s, NULL, NULL, &rows, &n));
  ASSERT_EQ_INT(n, 2);
  ASSERT_STR_EQ(rows[0].date, "2026-01-20");   /* newest first */
  ASSERT_MONEY_EQ(rows[0].amount, 30000);      /* entry size = debit total */
  ASSERT_STR_EQ(rows[0].status, "POSTED");
  ASSERT_STR_EQ(rows[0].flow, "EXPENSE");      /* Dr Expense / Cr bank */
  ASSERT_STR_EQ(rows[1].flow, "INCOME");       /* Dr bank / Cr Income */
  ASSERT_MONEY_EQ(rows[1].amount, 100000);
  free(rows);
  mb_store_close(s);
}

TEST(report, category_txns_income_and_expense) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_scenario(s, bank, income, expense);
  ASSERT_OK(post2(s, "2026-01-10", bank, income, 100000));   /* income 1000 */
  ASSERT_OK(post2(s, "2026-02-10", bank, income, 50000));    /* income 500  */
  ASSERT_OK(post2(s, "2026-01-20", expense, bank, 30000));   /* expense 300 */

  mb_cat_txn_row *rows = NULL; int n = 0;
  ASSERT_OK(mb_report_category_txns(s, "INCOME", NULL, NULL, &rows, &n));
  ASSERT_EQ_INT(n, 2);
  ASSERT_STR_EQ(rows[0].category_name, "Income");
  ASSERT_MONEY_EQ(rows[0].amount, 50000);     /* positive magnitude, newest first */
  ASSERT_MONEY_EQ(rows[1].amount, 100000);
  free(rows);

  ASSERT_OK(mb_report_category_txns(s, "EXPENSE", NULL, NULL, &rows, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_STR_EQ(rows[0].category_name, "Expense");
  ASSERT_MONEY_EQ(rows[0].amount, 30000);
  free(rows);
  mb_store_close(s);
}

/* audit F5: a reversed entry must net out everywhere. The journal (audit view) shows BOTH the
 * original and the REVERSAL; P&L / trial balance net to zero; and the "effective" category_txns
 * list excludes the reversed pair. */
TEST(report, reversed_entry_nets_out) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_scenario(s, bank, income, expense);

  /* post $1000 income, then reverse it */
  mb_posting_in p[] = {{.account_id = bank, .amount = 100000}, {.account_id = income, .amount = -100000}};
  char e1[40]; ASSERT_OK(mb_journal_post(s, "2026-01-10", "income", MB_SRC_USER, p, 2, e1));
  char rev[40]; ASSERT_OK(mb_journal_reverse(s, e1, rev));

  /* journal (audit trail) includes the original AND the reversal */
  mb_journal_row *jr = NULL; int jn = 0;
  ASSERT_OK(mb_report_journal(s, NULL, NULL, &jr, &jn));
  ASSERT_EQ_INT(jn, 2);
  int saw_reversal = 0;
  for (int i = 0; i < jn; i++) if (!strcmp(jr[i].status, "REVERSAL")) saw_reversal = 1;
  ASSERT_TRUE(saw_reversal);
  free(jr);

  /* P&L and trial balance net to zero — the reversal cancels the income */
  mb_pnl pnl; ASSERT_OK(mb_report_pnl(s, NULL, NULL, &pnl));
  ASSERT_MONEY_EQ(pnl.income, 0);
  mb_acct_balance *tbr = NULL; int tbn = 0; mb_trial_balance tb;
  ASSERT_OK(mb_report_trial_balance(s, NULL, &tbr, &tbn, &tb));
  ASSERT_TRUE(tb.balanced);
  free(tbr);

  /* the original entry is now flagged REVERSED (the postings stay; only the lifecycle flag changes) */
  ASSERT_OK(mb_report_journal(s, NULL, NULL, &jr, &jn));
  int saw_reversed = 0;
  for (int i = 0; i < jn; i++) if (!strcmp(jr[i].status, "REVERSED")) saw_reversed = 1;
  ASSERT_TRUE(saw_reversed);
  free(jr);

  /* the effective category list nets the reversed income out entirely (Concern C1 — fixed) */
  mb_cat_txn_row *rows = NULL; int n = 0;
  ASSERT_OK(mb_report_category_txns(s, "INCOME", NULL, NULL, &rows, &n));
  ASSERT_EQ_INT(n, 0);
  free(rows);
  mb_store_close(s);
}
#endif
