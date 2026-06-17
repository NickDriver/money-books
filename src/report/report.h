#ifndef MB_REPORT_H
#define MB_REPORT_H
/*
 * Money Books — report views (Phase 3, SPEC §6). Structured results reused by the
 * Reports section, dashboard widgets, and the MCP run_report tool.
 *
 * All amounts signed cents (+ debit-normal). Date window is inclusive; NULL = open end.
 */
#include "../store/store.h"
#include "../money/money.h"
#include "../account/account.h"

typedef struct {
  char            account_id[40];
  char            code[32];
  char            name[128];
  mb_account_type type;
  mb_money        balance;   /* signed: + = net debit, - = net credit */
} mb_acct_balance;

/* Per-account net posting over [from, to] (NULL = unbounded). Includes zero-balance accounts.
 * Allocates *rows (caller frees with free()). */
mb_err mb_report_balances(mb_store *s, const char *from, const char *to,
                          mb_acct_balance **rows, int *n) MB_MUST_CHECK;

/* Trial balance as of `as_of` (NULL = all time). debits must equal credits. */
typedef struct { mb_money total_debit, total_credit; int balanced; } mb_trial_balance;
mb_err mb_report_trial_balance(mb_store *s, const char *as_of,
                               mb_acct_balance **rows, int *n, mb_trial_balance *summary) MB_MUST_CHECK;

/* Profit & loss over [from, to]. */
typedef struct { mb_money income, expense, net; } mb_pnl;
mb_err mb_report_pnl(mb_store *s, const char *from, const char *to, mb_pnl *out) MB_MUST_CHECK;

/* Balance sheet as of `as_of`. Identity: assets == liabilities + equity + net_income. */
typedef struct { mb_money assets, liabilities, equity, net_income; int balanced; } mb_balance_sheet;
mb_err mb_report_balance_sheet(mb_store *s, const char *as_of, mb_balance_sheet *out) MB_MUST_CHECK;

/* General ledger: every posting hitting an account over [from, to], with a running balance
 * (seeded from the account's opening balance before `from`). Allocates *rows. */
typedef struct {
  char     entry_id[40];
  char     date[24];
  char     memo[256];
  mb_money amount;    /* signed */
  mb_money running;   /* running balance incl. this row */
} mb_ledger_row;
mb_err mb_report_ledger(mb_store *s, const char *account_id, const char *from, const char *to,
                        mb_ledger_row **rows, int *n) MB_MUST_CHECK;

/* AR/AP aging as of `as_of` (NULL = today), bucketed by age past due_date (or issue_date). */
typedef struct { mb_money current, d1_30, d31_60, d61_90, d90_plus, total; } mb_aging;
mb_err mb_report_ar_aging(mb_store *s, const char *as_of, mb_aging *out) MB_MUST_CHECK;
mb_err mb_report_ap_aging(mb_store *s, const char *as_of, mb_aging *out) MB_MUST_CHECK;

/* Pragmatic cash flow over [from, to]: movement across cash/bank accounts (ASSET + role ACCOUNT). */
typedef struct { mb_money inflow, outflow, net; } mb_cash_flow;
mb_err mb_report_cash_flow(mb_store *s, const char *from, const char *to, mb_cash_flow *out) MB_MUST_CHECK;

/* Transactions (journal) view: every recorded entry, newest first, with its size (sum of debit
 * postings). Includes reversals — this is the audit view. Allocates *rows; caller frees. */
typedef struct {
  char     entry_id[40];
  char     date[24];
  char     memo[256];
  char     source[8];    /* USER / AI / IMPORT */
  char     status[12];   /* POSTED / REVERSED / REVERSAL */
  char     flow[8];      /* INCOME / EXPENSE / OTHER — by which account types the entry touches */
  mb_money amount;       /* entry size = Σ positive (debit) postings */
} mb_journal_row;
mb_err mb_report_journal(mb_store *s, const char *from, const char *to,
                         mb_journal_row **rows, int *n) MB_MUST_CHECK;

/* Category transactions: posting-level income or expense activity over [from, to], newest first.
 * `acct_type` is "INCOME" or "EXPENSE". Only effective (POSTED) entries. Allocates *rows. */
typedef struct {
  char     entry_id[40];
  char     date[24];
  char     memo[256];
  char     category_id[40];
  char     category_name[128];
  mb_money amount;       /* positive magnitude */
} mb_cat_txn_row;
mb_err mb_report_category_txns(mb_store *s, const char *acct_type, const char *from, const char *to,
                               mb_cat_txn_row **rows, int *n) MB_MUST_CHECK;

#endif /* MB_REPORT_H */
