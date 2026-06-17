#include "payment.h"

#include <stdio.h>
#include <string.h>
#include "../support/mb_id.h"
#include "../journal/journal.h"

typedef struct {
  const char *kind_str;     /* "INVOICE" / "BILL" */
  const char *table;        /* "invoice" / "bill" */
  const char *line_table;   /* "invoice_line" / "bill_line" */
  const char *fk;           /* "invoice_id" / "bill_id" */
} target_info;

static target_info info_for(mb_pay_target t) {
  if (t == MB_PAY_BILL)
    return (target_info){"BILL", "bill", "bill_line", "bill_id"};
  return (target_info){"INVOICE", "invoice", "invoice_line", "invoice_id"};
}

static mb_err scalar_money(mb_store *s, const char *sql, const char *bind, mb_money *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), sql, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, bind, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_ROW) ? (*out = (mb_money)sqlite3_column_int64(st, 0), MB_OK)
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_payment_total_for(mb_store *s, mb_pay_target target, const char *target_id, mb_money *out) {
  target_info ti = info_for(target);
  char sql[128];
  snprintf(sql, sizeof sql,
           "SELECT COALESCE(SUM(amount),0) FROM payment WHERE target_kind='%s' AND target_id=?;",
           ti.kind_str);
  return scalar_money(s, sql, target_id, out);
}

static mb_err target_status(mb_store *s, const target_info *ti, const char *id, char buf[16]) {
  char sql[64];
  snprintf(sql, sizeof sql, "SELECT status FROM %s WHERE id=?;", ti->table);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), sql, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  mb_err e;
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW)       { snprintf(buf, 16, "%s", (const char *)sqlite3_column_text(st, 0)); e = MB_OK; }
  else if (rc == SQLITE_DONE) { e = MB_FAIL(MB_ERR_NOT_FOUND, "%s '%s'", ti->table, id); }
  else                        { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  sqlite3_finalize(st);
  return e;
}

/* counterparty_id of a document */
static mb_err target_counterparty(mb_store *s, const target_info *ti, const char *id, char buf[40]) {
  char sql[64];
  snprintf(sql, sizeof sql, "SELECT counterparty_id FROM %s WHERE id=?;", ti->table);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), sql, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  mb_err e;
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW)       { snprintf(buf, 40, "%s", (const char *)sqlite3_column_text(st, 0)); e = MB_OK; }
  else if (rc == SQLITE_DONE) { e = MB_FAIL(MB_ERR_NOT_FOUND, "%s '%s'", ti->table, id); }
  else                        { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  sqlite3_finalize(st);
  return e;
}

/* document line total (what is owed) */
static mb_err doc_total(mb_store *s, const target_info *ti, const char *id, mb_money *out) {
  char sql[128];
  snprintf(sql, sizeof sql, "SELECT COALESCE(SUM(line_total),0) FROM %s WHERE %s=?;", ti->line_table, ti->fk);
  return scalar_money(s, sql, id, out);
}

/* amount already settled against a document = Σ allocations to it */
static mb_err doc_settled(mb_store *s, const target_info *ti, const char *id, mb_money *out) {
  char sql[128];
  snprintf(sql, sizeof sql,
           "SELECT COALESCE(SUM(amount),0) FROM allocation WHERE target_kind='%s' AND target_id=?;", ti->kind_str);
  return scalar_money(s, sql, id, out);
}

/* recompute PARTIAL/PAID from allocations vs total (assumes caller holds a txn) */
static mb_err recompute_status(mb_store *s, const target_info *ti, const char *id) {
  mb_money settled = 0, total = 0;
  MB_TRY(doc_settled(s, ti, id, &settled));
  MB_TRY(doc_total(s, ti, id, &total));
  const char *newst = (settled >= total) ? "PAID" : "PARTIAL";
  char upd[64];
  snprintf(upd, sizeof upd, "UPDATE %s SET status=? WHERE id=?;", ti->table);
  sqlite3_stmt *u;
  if (sqlite3_prepare_v2(mb_store_handle(s), upd, -1, &u, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(u, 1, newst, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(u, 2, id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(u) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "update status: %s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(u);
  return e;
}

/* insert one allocation row (open-item settlement). Assumes caller holds a txn. */
static mb_err insert_allocation(mb_store *s, const char *date, const char *cp,
                                const char *source_kind, const char *payment_id,
                                const target_info *ti, const char *target_id, mb_money amount,
                                char id_out[40]) {
  char aid[40], created[24];
  MB_TRY(mb_uuid(aid, sizeof aid));
  mb_now_iso(created, sizeof created);
  int64_t seq, lamport;
  MB_TRY(mb_store_next_stamp(s, &seq, &lamport));
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO allocation(id,date,counterparty_id,source_kind,payment_id,target_kind,target_id,amount,"
        "created_at,device_id,seq,lamport) VALUES(?,?,?,?,?,?,?,?,?,"
        "(SELECT v FROM book_meta WHERE k='device_id'),?,?);", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, aid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, date, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, cp, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, source_kind, -1, SQLITE_TRANSIENT);
  if (payment_id) sqlite3_bind_text(st, 5, payment_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
  sqlite3_bind_text(st, 6, ti->kind_str, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, target_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 8, amount);
  sqlite3_bind_text(st, 9, created, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 10, seq);
  sqlite3_bind_int64(st, 11, lamport);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "insert allocation: %s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  if (e == MB_OK && id_out) snprintf(id_out, 40, "%s", aid);
  return e;
}

mb_err mb_payment_record(mb_store *s, const char *date, mb_money amount,
                         const char *cash_account_id, mb_pay_target target,
                         const char *target_id, char id_out[40]) {
  if (!date || !date[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "date required");
  if (amount <= 0) return MB_FAIL(MB_ERR_INVALID_ARG, "amount must be positive");
  if (!cash_account_id || !cash_account_id[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "cash account required");

  target_info ti = info_for(target);

  char status[16];
  MB_TRY(target_status(s, &ti, target_id, status));
  if (strcmp(status, "OPEN") != 0 && strcmp(status, "PARTIAL") != 0)
    return MB_FAIL(MB_ERR_INVALID_ARG, "%s is %s, not payable", ti.table, status);

  /* control account (AR for invoices, AP for bills) */
  char control[40];
  const char *meta_key = (target == MB_PAY_BILL) ? "ap_account_id" : "ar_account_id";
  if (mb_store_meta_get(s, meta_key, control, sizeof control) != MB_OK)
    return MB_FAIL(MB_ERR_INTERNAL, "control account not configured (seed the book)");

  /* counterparty of the document, for the AR/AP posting tag + payment denormalization (D26) */
  char cp[40];
  MB_TRY(target_counterparty(s, &ti, target_id, cp));

  /* how much of this cash actually settles the document; any excess becomes available credit */
  mb_money total = 0, settled = 0;
  MB_TRY(doc_total(s, &ti, target_id, &total));
  MB_TRY(doc_settled(s, &ti, target_id, &settled));
  mb_money remaining = total - settled;
  if (remaining < 0) remaining = 0;
  mb_money applied = (amount < remaining) ? amount : remaining;  /* overage → credit (not allocated) */

  /* postings: cash moves the full amount; the AR/AP leg is tagged to the counterparty */
  mb_posting_in p[2];
  if (target == MB_PAY_BILL) {  /* Dr AP, Cr Cash */
    p[0] = (mb_posting_in){.account_id = control,         .amount = amount,  .memo = NULL, .counterparty_id = cp};
    p[1] = (mb_posting_in){.account_id = cash_account_id, .amount = -amount, .memo = NULL, .counterparty_id = NULL};
  } else {                      /* Dr Cash, Cr AR */
    p[0] = (mb_posting_in){.account_id = cash_account_id, .amount = amount,  .memo = NULL, .counterparty_id = NULL};
    p[1] = (mb_posting_in){.account_id = control,         .amount = -amount, .memo = NULL, .counterparty_id = cp};
  }

  char pid[40], created[24];
  MB_TRY(mb_uuid(pid, sizeof pid));
  mb_now_iso(created, sizeof created);
  char memo[64];
  snprintf(memo, sizeof memo, "Payment %.40s", target_id);

  mb_err e = mb_store_begin(s);
  if (e != MB_OK) return e;

  int64_t seq, lamport;
  char entry_id[40];
  if ((e = mb_store_next_stamp(s, &seq, &lamport)) != MB_OK) goto done;
  if ((e = mb_journal_post_tx(s, date, memo, MB_SRC_USER, p, 2, entry_id)) != MB_OK) goto done;

  {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(mb_store_handle(s),
          "INSERT INTO payment(id,date,amount,account_id,target_kind,target_id,entry_id,created_at,"
          "device_id,seq,lamport,counterparty_id) VALUES(?,?,?,?,?,?,?,?,"
          "(SELECT v FROM book_meta WHERE k='device_id'),?,?,?);", -1, &st, NULL) != SQLITE_OK) {
      e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); goto done;
    }
    sqlite3_bind_text(st, 1, pid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, amount);
    sqlite3_bind_text(st, 4, cash_account_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, ti.kind_str, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, target_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, entry_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, created, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 9, seq);
    sqlite3_bind_int64(st, 10, lamport);
    sqlite3_bind_text(st, 11, cp, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { e = MB_FAIL(MB_ERR_DB, "insert payment: %s", sqlite3_errmsg(mb_store_handle(s))); goto done; }
  }

  /* allocate the applied portion to the document; recompute its status from allocations */
  if (applied > 0 && (e = insert_allocation(s, date, cp, "PAYMENT", pid, &ti, target_id, applied, NULL)) != MB_OK) goto done;
  if ((e = recompute_status(s, &ti, target_id)) != MB_OK) goto done;

done:
  if (e != MB_OK) { mb_store_rollback(s); return e; }
  e = mb_store_commit(s);
  if (e == MB_OK) snprintf(id_out, 40, "%s", pid);
  return e;
}

mb_err mb_counterparty_balance(mb_store *s, const char *counterparty_id, mb_pay_target target,
                               mb_money *out) {
  if (!counterparty_id || !counterparty_id[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "counterparty required");
  char control[40];
  const char *meta_key = (target == MB_PAY_BILL) ? "ap_account_id" : "ar_account_id";
  if (mb_store_meta_get(s, meta_key, control, sizeof control) != MB_OK)
    return MB_FAIL(MB_ERR_INTERNAL, "control account not configured (seed the book)");
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT COALESCE(SUM(amount),0) FROM posting WHERE account_id=? AND counterparty_id=?;",
        -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, control, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, counterparty_id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_ROW) ? (*out = (mb_money)sqlite3_column_int64(st, 0), MB_OK)
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_credit_available(mb_store *s, const char *counterparty_id, mb_pay_target target,
                           mb_money *out) {
  if (!counterparty_id || !counterparty_id[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "counterparty required");
  target_info ti = info_for(target);
  /* Σ this counterparty's cash payments on this side − Σ its allocations */
  char sql[256];
  snprintf(sql, sizeof sql,
    "SELECT "
    "(SELECT COALESCE(SUM(amount),0) FROM payment WHERE counterparty_id=?1 AND target_kind='%s') - "
    "(SELECT COALESCE(SUM(amount),0) FROM allocation WHERE counterparty_id=?1 AND target_kind='%s');",
    ti.kind_str, ti.kind_str);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), sql, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, counterparty_id, -1, SQLITE_TRANSIENT);
  mb_err e;
  if (sqlite3_step(st) == SQLITE_ROW) {
    mb_money v = (mb_money)sqlite3_column_int64(st, 0);
    *out = v < 0 ? 0 : v;
    e = MB_OK;
  } else {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  }
  sqlite3_finalize(st);
  return e;
}

mb_err mb_credit_apply(mb_store *s, const char *date, const char *counterparty_id,
                       mb_pay_target target, const char *target_id, mb_money amount,
                       char id_out[40]) {
  if (!date || !date[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "date required");
  if (amount <= 0) return MB_FAIL(MB_ERR_INVALID_ARG, "amount must be positive");
  if (!counterparty_id || !counterparty_id[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "counterparty required");

  target_info ti = info_for(target);

  /* target must be payable and belong to this counterparty */
  char status[16];
  MB_TRY(target_status(s, &ti, target_id, status));
  if (strcmp(status, "OPEN") != 0 && strcmp(status, "PARTIAL") != 0)
    return MB_FAIL(MB_ERR_INVALID_ARG, "%s is %s, not payable", ti.table, status);
  char cp[40];
  MB_TRY(target_counterparty(s, &ti, target_id, cp));
  if (strcmp(cp, counterparty_id) != 0)
    return MB_FAIL(MB_ERR_INVALID_ARG, "%s belongs to a different counterparty", ti.table);

  /* cannot exceed the document's remaining balance */
  mb_money total = 0, settled = 0;
  MB_TRY(doc_total(s, &ti, target_id, &total));
  MB_TRY(doc_settled(s, &ti, target_id, &settled));
  mb_money remaining = total - settled;
  if (amount > remaining) return MB_FAIL(MB_ERR_INVALID_ARG, "amount exceeds the remaining balance");

  /* cannot exceed available credit */
  mb_money avail = 0;
  MB_TRY(mb_credit_available(s, counterparty_id, target, &avail));
  if (amount > avail) return MB_FAIL(MB_ERR_INVALID_ARG, "amount exceeds available credit");

  char aid[40] = "";
  mb_err e = mb_store_begin(s);
  if (e != MB_OK) return e;
  if ((e = insert_allocation(s, date, counterparty_id, "CREDIT", NULL, &ti, target_id, amount, aid)) != MB_OK) goto fail;
  if ((e = recompute_status(s, &ti, target_id)) != MB_OK) goto fail;
  e = mb_store_commit(s);
  if (e == MB_OK) snprintf(id_out, 40, "%s", aid);
  return e;
fail:
  mb_store_rollback(s);
  return e;
}

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../account/account.h"
#include "../counterparty/counterparty.h"
#include "../invoice/invoice.h"
#include "../bill/bill.h"
#include "../seed/seed.h"

/* global posting sum — the books must always balance, even with credits floating around */
static mb_money g_posting_sum(mb_store *s) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(mb_store_handle(s), "SELECT COALESCE(SUM(amount),0) FROM posting;", -1, &st, NULL);
  sqlite3_step(st);
  mb_money v = (mb_money)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}
static int journal_entry_count(mb_store *s) {
  sqlite3_stmt *st;
  sqlite3_prepare_v2(mb_store_handle(s), "SELECT COUNT(*) FROM journal_entry;", -1, &st, NULL);
  sqlite3_step(st);
  int v = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return v;
}

TEST(payment, invoice_partial_then_paid) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40], bank[40];
  mb_account_new ai = {.code="4000", .name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  mb_account_new ab = {.code="1000", .name="Checking", .type=MB_ACCT_ASSET, .role=MB_ROLE_ACCOUNT};
  ASSERT_OK(mb_account_create(s, &ai, income));
  ASSERT_OK(mb_account_create(s, &ab, bank));
  char cp[40]; mb_counterparty_new c = {.name="Acme", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cp));

  char inv[40]; ASSERT_OK(mb_invoice_create(s, cp, "INV-9", NULL, NULL, inv));
  char lid[40];
  mb_invoice_line_in l = {.description="Work", .qty_centi=100, .unit_price=100000, .account_id=income};
  ASSERT_OK(mb_invoice_add_line(s, inv, &l, lid));
  ASSERT_OK(mb_invoice_issue(s, inv, "2026-07-01"));

  char ar[40]; ASSERT_OK(mb_store_meta_get(s, "ar_account_id", ar, sizeof ar));
  char pid[40];
  /* partial: $400 of $1000 */
  ASSERT_OK(mb_payment_record(s, "2026-07-10", 40000, bank, MB_PAY_INVOICE, inv, pid));
  mb_invoice got; ASSERT_OK(mb_invoice_get(s, inv, &got)); ASSERT_EQ_INT(got.status, MB_INV_PARTIAL);
  mb_money bal;
  ASSERT_OK(mb_account_balance(s, bank, &bal)); ASSERT_MONEY_EQ(bal, 40000);
  ASSERT_OK(mb_account_balance(s, ar, &bal));   ASSERT_MONEY_EQ(bal, 60000);  /* remaining */

  /* remainder: $600 */
  ASSERT_OK(mb_payment_record(s, "2026-07-20", 60000, bank, MB_PAY_INVOICE, inv, pid));
  ASSERT_OK(mb_invoice_get(s, inv, &got)); ASSERT_EQ_INT(got.status, MB_INV_PAID);
  ASSERT_OK(mb_account_balance(s, ar, &bal));   ASSERT_MONEY_EQ(bal, 0);
  ASSERT_OK(mb_account_balance(s, bank, &bal));  ASSERT_MONEY_EQ(bal, 100000);

  mb_money paid; ASSERT_OK(mb_payment_total_for(s, MB_PAY_INVOICE, inv, &paid));
  ASSERT_MONEY_EQ(paid, 100000);
  mb_store_close(s);
}

/* ---- input-contract tests (was the mislabeled `rejects_non_open_and_bad_amount`, audit F2) ----
 * These setup helpers are defined just below and shared with the D26 tests. */

/* small helpers to set up a seeded book with a bank + income/expense category + one party.
 * They take `t` so the assertion macros can report/abort on setup failure. */
static void seed_book(mb_test *t, mb_store *s, char bank[40], char income[40], char expense[40]) {
  ASSERT_OK(mb_seed_system_accounts(s));
  mb_account_new ab = {.code="1000", .name="Checking",  .type=MB_ACCT_ASSET,   .role=MB_ROLE_ACCOUNT};
  mb_account_new ai = {.code="4000", .name="Income",    .type=MB_ACCT_INCOME,  .role=MB_ROLE_CATEGORY};
  mb_account_new ae = {.code="6000", .name="Supplies",  .type=MB_ACCT_EXPENSE, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &ab, bank));
  ASSERT_OK(mb_account_create(s, &ai, income));
  ASSERT_OK(mb_account_create(s, &ae, expense));
}
static void make_issued_invoice(mb_test *t, mb_store *s, const char *cp, const char *income,
                                mb_money cents, const char *date, char inv[40]) {
  ASSERT_OK(mb_invoice_create(s, cp, NULL, NULL, NULL, inv));
  char lid[40];
  mb_invoice_line_in l = {.description="Work", .qty_centi=100, .unit_price=cents, .account_id=income};
  ASSERT_OK(mb_invoice_add_line(s, inv, &l, lid));
  ASSERT_OK(mb_invoice_issue(s, inv, date));
}
static void make_entered_bill(mb_test *t, mb_store *s, const char *cp, const char *expense,
                              mb_money cents, const char *date, char bill[40]) {
  ASSERT_OK(mb_bill_create(s, cp, NULL, NULL, NULL, bill));
  char lid[40];
  mb_bill_line_in l = {.description="Parts", .qty_centi=100, .unit_price=cents, .account_id=expense};
  ASSERT_OK(mb_bill_add_line(s, bill, &l, lid));
  ASSERT_OK(mb_bill_enter(s, bill, date));
}

/* A payment may only be recorded against a payable (OPEN/PARTIAL) document — not a DRAFT or a
 * PAID one — on BOTH the invoice (AR) and the bill (AP) side. */
TEST(payment, rejects_payment_on_non_payable) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_book(t, s, bank, income, expense);
  char cust[40]; mb_counterparty_new c = {.name="Acme", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cust));
  char vend[40]; mb_counterparty_new v = {.name="Supplier", .kind=MB_CP_VENDOR};
  ASSERT_OK(mb_counterparty_create(s, &v, vend));
  char pid[40];

  /* invoice side: DRAFT is not payable */
  char draft_inv[40]; ASSERT_OK(mb_invoice_create(s, cust, NULL, NULL, NULL, draft_inv));
  ASSERT_ERR(mb_payment_record(s, "2026-07-01", 5000, bank, MB_PAY_INVOICE, draft_inv, pid), MB_ERR_INVALID_ARG);

  /* invoice side: a fully PAID invoice is not payable again */
  char inv[40]; make_issued_invoice(t, s, cust, income, 10000, "2026-07-01", inv);
  ASSERT_OK(mb_payment_record(s, "2026-07-05", 10000, bank, MB_PAY_INVOICE, inv, pid));   /* → PAID */
  ASSERT_ERR(mb_payment_record(s, "2026-07-06", 1000, bank, MB_PAY_INVOICE, inv, pid), MB_ERR_INVALID_ARG);

  /* bill side: DRAFT is not payable (the AP path was previously untested) */
  char draft_bill[40]; ASSERT_OK(mb_bill_create(s, vend, NULL, NULL, NULL, draft_bill));
  ASSERT_ERR(mb_payment_record(s, "2026-07-01", 5000, bank, MB_PAY_BILL, draft_bill, pid), MB_ERR_INVALID_ARG);

  /* bill side: a fully PAID bill is not payable again */
  char bill[40]; make_entered_bill(t, s, vend, expense, 8000, "2026-07-01", bill);
  ASSERT_OK(mb_payment_record(s, "2026-07-05", 8000, bank, MB_PAY_BILL, bill, pid));      /* → PAID */
  ASSERT_ERR(mb_payment_record(s, "2026-07-06", 1000, bank, MB_PAY_BILL, bill, pid), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}

/* The amount must be strictly positive (zero and negative are rejected), and the required string
 * inputs (date, cash account) must be present. Asserted on a genuinely OPEN invoice so the ONLY
 * thing that can reject the call is the input being tested. */
TEST(payment, rejects_non_positive_amount_and_missing_inputs) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_book(t, s, bank, income, expense);
  char cust[40]; mb_counterparty_new c = {.name="Acme", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cust));
  char inv[40]; make_issued_invoice(t, s, cust, income, 50000, "2026-07-01", inv);   /* OPEN, $500 owed */
  char pid[40];

  /* zero and negative amounts are invalid */
  ASSERT_ERR(mb_payment_record(s, "2026-07-05", 0,    bank, MB_PAY_INVOICE, inv, pid), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_payment_record(s, "2026-07-05", -100, bank, MB_PAY_INVOICE, inv, pid), MB_ERR_INVALID_ARG);
  /* missing date / cash account are invalid */
  ASSERT_ERR(mb_payment_record(s, NULL, 1000, bank, MB_PAY_INVOICE, inv, pid), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_payment_record(s, "",   1000, bank, MB_PAY_INVOICE, inv, pid), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_payment_record(s, "2026-07-05", 1000, NULL, MB_PAY_INVOICE, inv, pid), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_payment_record(s, "2026-07-05", 1000, "",   MB_PAY_INVOICE, inv, pid), MB_ERR_INVALID_ARG);

  /* none of the rejected calls should have changed anything — invoice still fully OPEN, $0 settled */
  mb_invoice got; ASSERT_OK(mb_invoice_get(s, inv, &got)); ASSERT_EQ_INT(got.status, MB_INV_OPEN);
  mb_money paid; ASSERT_OK(mb_payment_total_for(s, MB_PAY_INVOICE, inv, &paid));
  ASSERT_MONEY_EQ(paid, 0);
  mb_store_close(s);
}

/* Overpayment is ALLOWED by design (D26): paying more than the balance succeeds and marks the
 * document PAID. This pins the decision so a future "helpful" over-balance guard would fail here.
 * (The resulting credit mechanics are proven by the D26 tests below.) */
TEST(payment, allows_overpayment) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_book(t, s, bank, income, expense);
  char cust[40]; mb_counterparty_new c = {.name="Acme", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cust));
  char inv[40]; make_issued_invoice(t, s, cust, income, 10000, "2026-07-01", inv);   /* $100 owed */
  char pid[40];
  ASSERT_OK(mb_payment_record(s, "2026-07-05", 15000, bank, MB_PAY_INVOICE, inv, pid));  /* pay $150 */
  mb_invoice got; ASSERT_OK(mb_invoice_get(s, inv, &got)); ASSERT_EQ_INT(got.status, MB_INV_PAID);
  mb_money credit; ASSERT_OK(mb_credit_available(s, cust, MB_PAY_INVOICE, &credit));
  ASSERT_MONEY_EQ(credit, 5000);   /* the $50 excess is real, available credit */
  mb_store_close(s);
}

/* ---- D26: customer/vendor credit (balance-forward + manual application) ---- */

TEST(payment, overpay_records_customer_credit) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_book(t, s, bank, income, expense);
  char cp[40]; mb_counterparty_new c = {.name="Acme", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cp));
  char ar[40]; ASSERT_OK(mb_store_meta_get(s, "ar_account_id", ar, sizeof ar));

  /* $400 invoice, customer pays $1000 */
  char inv[40]; make_issued_invoice(t, s, cp, income, 40000, "2026-07-01", inv);
  char pid[40];
  ASSERT_OK(mb_payment_record(s, "2026-07-10", 100000, bank, MB_PAY_INVOICE, inv, pid));

  /* document is fully settled (only $400 of the cash was allocated to it) */
  mb_invoice got; ASSERT_OK(mb_invoice_get(s, inv, &got)); ASSERT_EQ_INT(got.status, MB_INV_PAID);

  /* the $600 excess is the customer's available credit, and shows as a negative AR balance */
  mb_money avail; ASSERT_OK(mb_credit_available(s, cp, MB_PAY_INVOICE, &avail));
  ASSERT_MONEY_EQ(avail, 60000);
  mb_money cpbal; ASSERT_OK(mb_counterparty_balance(s, cp, MB_PAY_INVOICE, &cpbal));
  ASSERT_MONEY_EQ(cpbal, -60000);                 /* debit-normal AR: negative = customer in credit */
  mb_money arbal; ASSERT_OK(mb_account_balance(s, ar, &arbal));
  ASSERT_MONEY_EQ(arbal, -60000);                 /* whole-AR control agrees (single customer) */
  mb_money cash; ASSERT_OK(mb_account_balance(s, bank, &cash));
  ASSERT_MONEY_EQ(cash, 100000);                  /* all cash really came in */
  ASSERT_MONEY_EQ(g_posting_sum(s), 0);           /* books balance */
  mb_store_close(s);
}

TEST(payment, customer_credit_applies_to_new_invoice_manually) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_book(t, s, bank, income, expense);
  char cp[40]; mb_counterparty_new c = {.name="Acme", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cp));

  /* prepay: $400 invoice, $1000 cash → $600 credit */
  char inv1[40]; make_issued_invoice(t, s, cp, income, 40000, "2026-07-01", inv1);
  char pid[40]; ASSERT_OK(mb_payment_record(s, "2026-07-10", 100000, bank, MB_PAY_INVOICE, inv1, pid));
  int entries_before = journal_entry_count(s);

  /* a new $300 invoice — still OPEN until we apply credit (manual, not automatic) */
  char inv2[40]; make_issued_invoice(t, s, cp, income, 30000, "2026-08-01", inv2);
  mb_invoice g2; ASSERT_OK(mb_invoice_get(s, inv2, &g2)); ASSERT_EQ_INT(g2.status, MB_INV_OPEN);

  /* apply $300 of the credit to invoice 2 — no cash, no new journal entry */
  int entries_after_issue = journal_entry_count(s);
  char aid[40]; ASSERT_OK(mb_credit_apply(s, "2026-08-02", cp, MB_PAY_INVOICE, inv2, 30000, aid));
  ASSERT_OK(mb_invoice_get(s, inv2, &g2)); ASSERT_EQ_INT(g2.status, MB_INV_PAID);
  ASSERT_EQ_INT(journal_entry_count(s), entries_after_issue);   /* credit application posts NO entry */

  /* $300 credit consumed; $300 remains */
  mb_money avail; ASSERT_OK(mb_credit_available(s, cp, MB_PAY_INVOICE, &avail));
  ASSERT_MONEY_EQ(avail, 30000);
  /* AR balance = 400 + 300 invoiced − 1000 paid = −300 */
  mb_money cpbal; ASSERT_OK(mb_counterparty_balance(s, cp, MB_PAY_INVOICE, &cpbal));
  ASSERT_MONEY_EQ(cpbal, -30000);
  ASSERT_MONEY_EQ(g_posting_sum(s), 0);
  (void)entries_before;
  mb_store_close(s);
}

TEST(payment, credit_apply_validations) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_book(t, s, bank, income, expense);
  char cp[40]; mb_counterparty_new c = {.name="Acme", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cp));
  char cp2[40]; mb_counterparty_new c2 = {.name="Beta", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c2, cp2));

  /* $400 invoice paid $1000 → $600 credit for cp */
  char inv1[40]; make_issued_invoice(t, s, cp, income, 40000, "2026-07-01", inv1);
  char pid[40]; ASSERT_OK(mb_payment_record(s, "2026-07-10", 100000, bank, MB_PAY_INVOICE, inv1, pid));

  char inv2[40]; make_issued_invoice(t, s, cp, income, 30000, "2026-08-01", inv2);
  char aid[40];
  /* non-positive amount */
  ASSERT_ERR(mb_credit_apply(s, "2026-08-02", cp, MB_PAY_INVOICE, inv2, 0, aid), MB_ERR_INVALID_ARG);
  /* exceeds the document's remaining balance ($300) */
  ASSERT_ERR(mb_credit_apply(s, "2026-08-02", cp, MB_PAY_INVOICE, inv2, 40000, aid), MB_ERR_INVALID_ARG);
  /* belongs to a different counterparty (cp2 has no claim on cp's invoice) */
  ASSERT_ERR(mb_credit_apply(s, "2026-08-02", cp2, MB_PAY_INVOICE, inv2, 10000, aid), MB_ERR_INVALID_ARG);
  /* a fresh DRAFT invoice is not payable */
  char inv3[40]; ASSERT_OK(mb_invoice_create(s, cp, NULL, NULL, NULL, inv3));
  ASSERT_ERR(mb_credit_apply(s, "2026-08-02", cp, MB_PAY_INVOICE, inv3, 10000, aid), MB_ERR_INVALID_ARG);

  /* cp2 has zero credit, so applying any credit to cp2's own open invoice fails on availability */
  char inv4[40]; make_issued_invoice(t, s, cp2, income, 5000, "2026-08-03", inv4);
  ASSERT_ERR(mb_credit_apply(s, "2026-08-04", cp2, MB_PAY_INVOICE, inv4, 5000, aid), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}

TEST(payment, credit_is_per_counterparty) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_book(t, s, bank, income, expense);
  char a[40]; mb_counterparty_new ca = {.name="Acme", .kind=MB_CP_CUSTOMER};
  char b[40]; mb_counterparty_new cb = {.name="Beta", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &ca, a));
  ASSERT_OK(mb_counterparty_create(s, &cb, b));

  /* Acme overpays $100 on a $100 invoice (no credit); Beta overpays $500 on a $100 invoice */
  char ia[40]; make_issued_invoice(t, s, a, income, 10000, "2026-07-01", ia);
  char pid[40]; ASSERT_OK(mb_payment_record(s, "2026-07-05", 10000, bank, MB_PAY_INVOICE, ia, pid));
  char ib[40]; make_issued_invoice(t, s, b, income, 10000, "2026-07-01", ib);
  ASSERT_OK(mb_payment_record(s, "2026-07-05", 60000, bank, MB_PAY_INVOICE, ib, pid));

  mb_money av_a, av_b;
  ASSERT_OK(mb_credit_available(s, a, MB_PAY_INVOICE, &av_a)); ASSERT_MONEY_EQ(av_a, 0);
  ASSERT_OK(mb_credit_available(s, b, MB_PAY_INVOICE, &av_b)); ASSERT_MONEY_EQ(av_b, 50000);
  /* Acme cannot spend Beta's credit */
  char aid[40];
  char ia2[40]; make_issued_invoice(t, s, a, income, 20000, "2026-08-01", ia2);
  ASSERT_ERR(mb_credit_apply(s, "2026-08-02", a, MB_PAY_INVOICE, ia2, 20000, aid), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}

TEST(payment, vendor_overpay_and_credit_apply) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], expense[40]; seed_book(t, s, bank, income, expense);
  char v[40]; mb_counterparty_new cv = {.name="Supplier", .kind=MB_CP_VENDOR};
  ASSERT_OK(mb_counterparty_create(s, &cv, v));
  char ap[40]; ASSERT_OK(mb_store_meta_get(s, "ap_account_id", ap, sizeof ap));

  /* $200 bill, we pay $500 → $300 vendor credit */
  char bill[40]; ASSERT_OK(mb_bill_create(s, v, NULL, NULL, NULL, bill));
  char lid[40];
  mb_bill_line_in bl = {.description="Parts", .qty_centi=100, .unit_price=20000, .account_id=expense};
  ASSERT_OK(mb_bill_add_line(s, bill, &bl, lid));
  ASSERT_OK(mb_bill_enter(s, bill, "2026-07-01"));
  char pid[40]; ASSERT_OK(mb_payment_record(s, "2026-07-10", 50000, bank, MB_PAY_BILL, bill, pid));

  mb_bill gb; ASSERT_OK(mb_bill_get(s, bill, &gb)); ASSERT_EQ_INT(gb.status, MB_BILL_PAID);
  mb_money avail; ASSERT_OK(mb_credit_available(s, v, MB_PAY_BILL, &avail));
  ASSERT_MONEY_EQ(avail, 30000);
  /* AP control sum (debit-positive): −200 (bill) + 500 (payment) = +300 = our credit with vendor */
  mb_money apbal; ASSERT_OK(mb_account_balance(s, ap, &apbal)); ASSERT_MONEY_EQ(apbal, 30000);
  ASSERT_MONEY_EQ(g_posting_sum(s), 0);

  /* apply $300 credit to a new $300 bill — no cash leaves the bank */
  char b2[40]; ASSERT_OK(mb_bill_create(s, v, NULL, NULL, NULL, b2));
  mb_bill_line_in bl2 = {.description="More", .qty_centi=100, .unit_price=30000, .account_id=expense};
  ASSERT_OK(mb_bill_add_line(s, b2, &bl2, lid));
  ASSERT_OK(mb_bill_enter(s, b2, "2026-08-01"));
  mb_money cash_before; ASSERT_OK(mb_account_balance(s, bank, &cash_before));
  char aid[40]; ASSERT_OK(mb_credit_apply(s, "2026-08-02", v, MB_PAY_BILL, b2, 30000, aid));
  ASSERT_OK(mb_bill_get(s, b2, &gb)); ASSERT_EQ_INT(gb.status, MB_BILL_PAID);
  mb_money cash_after; ASSERT_OK(mb_account_balance(s, bank, &cash_after));
  ASSERT_MONEY_EQ(cash_after, cash_before);       /* credit application moves no cash */
  ASSERT_OK(mb_credit_available(s, v, MB_PAY_BILL, &avail)); ASSERT_MONEY_EQ(avail, 0);
  ASSERT_MONEY_EQ(g_posting_sum(s), 0);
  mb_store_close(s);
}
#endif
