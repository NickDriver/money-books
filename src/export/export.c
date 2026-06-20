#include "export.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../report/report.h"
#include "../account/account.h"
#include "../money/money.h"

/* ---- growable output buffer (OOM is sticky; checked once at finish) ---- */
typedef struct { char *buf; size_t len, cap; int oom; } sb;

static void sb_raw(sb *b, const char *s, size_t n) {
  if (b->oom) return;
  if (b->len + n + 1 > b->cap) {
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + n + 1) ncap *= 2;
    char *p = realloc(b->buf, ncap);
    if (!p) { b->oom = 1; return; }
    b->buf = p; b->cap = ncap;
  }
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

/* one CSV field, RFC-4180 escaped, comma-separated by column position */
static void csv_text(sb *b, int *col, const char *s) {
  if ((*col)++ > 0) sb_raw(b, ",", 1);
  int quote = 0;
  for (const char *p = s; *p; p++)
    if (*p == '"' || *p == ',' || *p == '\n' || *p == '\r') { quote = 1; break; }
  if (!quote) { sb_raw(b, s, strlen(s)); return; }
  sb_raw(b, "\"", 1);
  for (const char *p = s; *p; p++) {
    if (*p == '"') sb_raw(b, "\"\"", 2);
    else sb_raw(b, p, 1);
  }
  sb_raw(b, "\"", 1);
}

static void csv_money(sb *b, int *col, mb_money v) {
  char m[32];
  if (mb_money_format(v, m, sizeof m) != MB_OK) snprintf(m, sizeof m, "0.00");
  csv_text(b, col, m);   /* plain decimal, no thousands sep -> imports as a number */
}

static void csv_eol(sb *b, int *col) { sb_raw(b, "\n", 1); *col = 0; }

static mb_err sb_finish(sb *b, char **out) {
  if (b->oom) { free(b->buf); return MB_FAIL(MB_ERR_INTERNAL, "oom building csv"); }
  if (!b->buf) { b->buf = calloc(1, 1); if (!b->buf) return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  *out = b->buf;
  return MB_OK;
}

/* ---- exporters ---- */

mb_err mb_export_trial_balance_csv(mb_store *s, const char *as_of, char **out) {
  mb_acct_balance *rows = NULL; int n = 0; mb_trial_balance tb;
  MB_TRY(mb_report_trial_balance(s, as_of, &rows, &n, &tb));

  sb b = {0}; int c = 0;
  csv_text(&b, &c, "Account Code"); csv_text(&b, &c, "Account Name"); csv_text(&b, &c, "Type");
  csv_text(&b, &c, "Debit"); csv_text(&b, &c, "Credit"); csv_eol(&b, &c);
  for (int i = 0; i < n; i++) {
    if (rows[i].balance == 0) continue;
    mb_money deb = rows[i].balance > 0 ? rows[i].balance : 0;
    mb_money cre = rows[i].balance < 0 ? -rows[i].balance : 0;
    csv_text(&b, &c, rows[i].code); csv_text(&b, &c, rows[i].name);
    csv_text(&b, &c, mb_account_type_str(rows[i].type));
    csv_money(&b, &c, deb); csv_money(&b, &c, cre); csv_eol(&b, &c);
  }
  csv_text(&b, &c, ""); csv_text(&b, &c, ""); csv_text(&b, &c, "TOTAL");
  csv_money(&b, &c, tb.total_debit); csv_money(&b, &c, tb.total_credit); csv_eol(&b, &c);
  free(rows);
  return sb_finish(&b, out);
}

mb_err mb_export_pnl_csv(mb_store *s, const char *from, const char *to, char **out) {
  mb_acct_balance *rows = NULL; int n = 0;
  MB_TRY(mb_report_balances(s, from, to, &rows, &n));

  sb b = {0}; int c = 0;
  csv_text(&b, &c, "Section"); csv_text(&b, &c, "Account Code");
  csv_text(&b, &c, "Account Name"); csv_text(&b, &c, "Amount"); csv_eol(&b, &c);

  mb_money income = 0, expense = 0;
  for (int i = 0; i < n; i++) {
    if (rows[i].type != MB_ACCT_INCOME) continue;
    mb_money amt = -rows[i].balance;          /* income is credit-normal */
    if (amt == 0) continue;
    csv_text(&b, &c, "Income"); csv_text(&b, &c, rows[i].code);
    csv_text(&b, &c, rows[i].name); csv_money(&b, &c, amt); csv_eol(&b, &c);
    income += amt;
  }
  csv_text(&b, &c, "Income"); csv_text(&b, &c, ""); csv_text(&b, &c, "Total Income");
  csv_money(&b, &c, income); csv_eol(&b, &c);

  for (int i = 0; i < n; i++) {
    if (rows[i].type != MB_ACCT_EXPENSE) continue;
    mb_money amt = rows[i].balance;           /* expense is debit-normal */
    if (amt == 0) continue;
    csv_text(&b, &c, "Expense"); csv_text(&b, &c, rows[i].code);
    csv_text(&b, &c, rows[i].name); csv_money(&b, &c, amt); csv_eol(&b, &c);
    expense += amt;
  }
  csv_text(&b, &c, "Expense"); csv_text(&b, &c, ""); csv_text(&b, &c, "Total Expense");
  csv_money(&b, &c, expense); csv_eol(&b, &c);

  csv_text(&b, &c, ""); csv_text(&b, &c, ""); csv_text(&b, &c, "Net Income");
  csv_money(&b, &c, income - expense); csv_eol(&b, &c);
  free(rows);
  return sb_finish(&b, out);
}

mb_err mb_export_balance_sheet_csv(mb_store *s, const char *as_of, char **out) {
  mb_balance_sheet bs;
  MB_TRY(mb_report_balance_sheet(s, as_of, &bs));
  mb_acct_balance *rows = NULL; int n = 0;
  MB_TRY(mb_report_balances(s, NULL, as_of, &rows, &n));   /* cumulative through as_of */

  sb b = {0}; int c = 0;
  csv_text(&b, &c, "Section"); csv_text(&b, &c, "Account Code");
  csv_text(&b, &c, "Account Name"); csv_text(&b, &c, "Amount"); csv_eol(&b, &c);

  for (int i = 0; i < n; i++) {
    if (rows[i].type != MB_ACCT_ASSET || rows[i].balance == 0) continue;
    csv_text(&b, &c, "Asset"); csv_text(&b, &c, rows[i].code);
    csv_text(&b, &c, rows[i].name); csv_money(&b, &c, rows[i].balance); csv_eol(&b, &c);
  }
  csv_text(&b, &c, "Asset"); csv_text(&b, &c, ""); csv_text(&b, &c, "Total Assets");
  csv_money(&b, &c, bs.assets); csv_eol(&b, &c);

  for (int i = 0; i < n; i++) {
    if (rows[i].type != MB_ACCT_LIABILITY || rows[i].balance == 0) continue;
    csv_text(&b, &c, "Liability"); csv_text(&b, &c, rows[i].code);
    csv_text(&b, &c, rows[i].name); csv_money(&b, &c, -rows[i].balance); csv_eol(&b, &c);
  }
  csv_text(&b, &c, "Liability"); csv_text(&b, &c, ""); csv_text(&b, &c, "Total Liabilities");
  csv_money(&b, &c, bs.liabilities); csv_eol(&b, &c);

  for (int i = 0; i < n; i++) {
    if (rows[i].type != MB_ACCT_EQUITY || rows[i].balance == 0) continue;
    csv_text(&b, &c, "Equity"); csv_text(&b, &c, rows[i].code);
    csv_text(&b, &c, rows[i].name); csv_money(&b, &c, -rows[i].balance); csv_eol(&b, &c);
  }
  csv_text(&b, &c, "Equity"); csv_text(&b, &c, ""); csv_text(&b, &c, "Net Income (current period)");
  csv_money(&b, &c, bs.net_income); csv_eol(&b, &c);
  csv_text(&b, &c, "Equity"); csv_text(&b, &c, ""); csv_text(&b, &c, "Total Equity + Net Income");
  csv_money(&b, &c, bs.equity + bs.net_income); csv_eol(&b, &c);

  csv_text(&b, &c, ""); csv_text(&b, &c, ""); csv_text(&b, &c, "Total Liabilities + Equity + Net Income");
  csv_money(&b, &c, bs.liabilities + bs.equity + bs.net_income); csv_eol(&b, &c);
  free(rows);
  return sb_finish(&b, out);
}

mb_err mb_export_general_ledger_csv(mb_store *s, const char *from, const char *to, char **out) {
  mb_account *accts = NULL; int an = 0;
  mb_account_filter f = {0};
  MB_TRY(mb_account_list(s, &f, &accts, &an));

  sb b = {0}; int c = 0;
  csv_text(&b, &c, "Account Code"); csv_text(&b, &c, "Account Name"); csv_text(&b, &c, "Date");
  csv_text(&b, &c, "Entry"); csv_text(&b, &c, "Memo"); csv_text(&b, &c, "Amount");
  csv_text(&b, &c, "Balance"); csv_eol(&b, &c);

  mb_err e = MB_OK;
  for (int i = 0; i < an && e == MB_OK; i++) {
    mb_ledger_row *lr = NULL; int ln = 0;
    e = mb_report_ledger(s, accts[i].id, from, to, &lr, &ln);
    if (e != MB_OK) break;
    for (int j = 0; j < ln; j++) {
      char ref[9];
      snprintf(ref, sizeof ref, "%.8s", lr[j].entry_id);   /* short, stable reference */
      csv_text(&b, &c, accts[i].code); csv_text(&b, &c, accts[i].name);
      csv_text(&b, &c, lr[j].date); csv_text(&b, &c, ref); csv_text(&b, &c, lr[j].memo);
      csv_money(&b, &c, lr[j].amount); csv_money(&b, &c, lr[j].running); csv_eol(&b, &c);
    }
    free(lr);
  }
  free(accts);
  if (e != MB_OK) { free(b.buf); return e; }
  return sb_finish(&b, out);
}

mb_err mb_export_journal_csv(mb_store *s, const char *from, const char *to, char **out) {
  mb_journal_row *rows = NULL; int n = 0;
  MB_TRY(mb_report_journal(s, from, to, &rows, &n));

  sb b = {0}; int c = 0;
  csv_text(&b, &c, "Date"); csv_text(&b, &c, "Entry"); csv_text(&b, &c, "Memo");
  csv_text(&b, &c, "Source"); csv_text(&b, &c, "Status"); csv_text(&b, &c, "Flow");
  csv_text(&b, &c, "Amount"); csv_eol(&b, &c);
  for (int i = 0; i < n; i++) {
    char ref[9];
    snprintf(ref, sizeof ref, "%.8s", rows[i].entry_id);
    csv_text(&b, &c, rows[i].date); csv_text(&b, &c, ref); csv_text(&b, &c, rows[i].memo);
    csv_text(&b, &c, rows[i].source); csv_text(&b, &c, rows[i].status); csv_text(&b, &c, rows[i].flow);
    csv_money(&b, &c, rows[i].amount); csv_eol(&b, &c);
  }
  free(rows);
  return sb_finish(&b, out);
}

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../journal/journal.h"

/* tiny book: Cash (asset), Consulting (income), Software (expense) + two entries */
static void seed_books(mb_store *s, char cash[40], char inc[40], char exp[40]) {
  mb_account_new a = {.code = "1000", .name = "Cash", .type = MB_ACCT_ASSET, .role = MB_ROLE_ACCOUNT};
  mb_account_new b = {.code = "4000", .name = "Consulting", .type = MB_ACCT_INCOME, .role = MB_ROLE_CATEGORY};
  mb_account_new d = {.code = "6000", .name = "Software, Inc", .type = MB_ACCT_EXPENSE, .role = MB_ROLE_CATEGORY};
  (void)mb_account_create(s, &a, cash);
  (void)mb_account_create(s, &b, inc);
  (void)mb_account_create(s, &d, exp);
  char e[40];
  mb_posting_in income[] = {{.account_id = cash, .amount = 50000}, {.account_id = inc, .amount = -50000}};
  (void)mb_journal_post(s, "2026-03-01", "March retainer", MB_SRC_USER, income, 2, e);
  mb_posting_in spend[] = {{.account_id = exp, .amount = 12000}, {.account_id = cash, .amount = -12000}};
  (void)mb_journal_post(s, "2026-03-05", "IDE license", MB_SRC_USER, spend, 2, e);
}

TEST(export, trial_balance_has_rows_and_total) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], inc[40], exp[40]; seed_books(s, cash, inc, exp);
  char *csv = NULL; ASSERT_OK(mb_export_trial_balance_csv(s, NULL, &csv));
  ASSERT_TRUE(strstr(csv, "Account Code,Account Name,Type,Debit,Credit\n") == csv);
  ASSERT_TRUE(strstr(csv, "1000,Cash") != NULL);
  /* debits == credits: Cash 380 + Software 120 = 500 debit; Consulting 500 credit */
  ASSERT_TRUE(strstr(csv, ",TOTAL,500.00,500.00\n") != NULL);
  free(csv);
  mb_store_close(s);
}

TEST(export, pnl_net_matches_report) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], inc[40], exp[40]; seed_books(s, cash, inc, exp);
  char *csv = NULL; ASSERT_OK(mb_export_pnl_csv(s, NULL, NULL, &csv));
  mb_pnl pl; ASSERT_OK(mb_report_pnl(s, NULL, NULL, &pl));
  ASSERT_MONEY_EQ(pl.net, 38000);                       /* 50000 income - 12000 expense */
  ASSERT_TRUE(strstr(csv, "Net Income,380.00\n") != NULL);
  ASSERT_TRUE(strstr(csv, "Income,4000,Consulting,500.00\n") != NULL);
  free(csv);
  mb_store_close(s);
}

TEST(export, general_ledger_lists_postings) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], inc[40], exp[40]; seed_books(s, cash, inc, exp);
  char *csv = NULL; ASSERT_OK(mb_export_general_ledger_csv(s, NULL, NULL, &csv));
  ASSERT_TRUE(strstr(csv, "Account Code,Account Name,Date,Entry,Memo,Amount,Balance\n") == csv);
  ASSERT_TRUE(strstr(csv, "March retainer") != NULL);
  ASSERT_TRUE(strstr(csv, "IDE license") != NULL);
  free(csv);
  mb_store_close(s);
}

TEST(export, csv_escapes_commas_in_names) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], inc[40], exp[40]; seed_books(s, cash, inc, exp);
  char *csv = NULL; ASSERT_OK(mb_export_trial_balance_csv(s, NULL, &csv));
  /* "Software, Inc" contains a comma -> must be quoted */
  ASSERT_TRUE(strstr(csv, "\"Software, Inc\"") != NULL);
  free(csv);
  mb_store_close(s);
}

TEST(export, journal_lists_entries) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], inc[40], exp[40]; seed_books(s, cash, inc, exp);
  char *csv = NULL; ASSERT_OK(mb_export_journal_csv(s, NULL, NULL, &csv));
  ASSERT_TRUE(strstr(csv, "Date,Entry,Memo,Source,Status,Flow,Amount\n") == csv);
  ASSERT_TRUE(strstr(csv, "March retainer") != NULL);
  free(csv);
  mb_store_close(s);
}
#endif
