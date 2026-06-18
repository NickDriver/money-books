#include "bill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../support/mb_id.h"
#include "../journal/journal.h"

const char *mb_bill_status_str(mb_bill_status st) {
  switch (st) {
    case MB_BILL_DRAFT:   return "DRAFT";
    case MB_BILL_OPEN:    return "OPEN";
    case MB_BILL_PARTIAL: return "PARTIAL";
    case MB_BILL_PAID:    return "PAID";
    case MB_BILL_VOID:    return "VOID";
  }
  return "DRAFT";
}

static mb_bill_status parse_status(const char *s) {
  if (!strcmp(s, "OPEN"))    return MB_BILL_OPEN;
  if (!strcmp(s, "PARTIAL")) return MB_BILL_PARTIAL;
  if (!strcmp(s, "PAID"))    return MB_BILL_PAID;
  if (!strcmp(s, "VOID"))    return MB_BILL_VOID;
  return MB_BILL_DRAFT;
}

mb_err mb_bill_create(mb_store *s, const char *counterparty_id, const char *number,
                      const char *due_date, const char *memo, char id_out[40]) {
  if (!counterparty_id || !counterparty_id[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "counterparty required");
  char id[40], created[24], currency[8];
  MB_TRY(mb_uuid(id, sizeof id));
  mb_now_iso(created, sizeof created);
  MB_TRY(mb_store_currency(s, currency));

  MB_TRY(mb_store_begin(s));
  int64_t seq, lamport;
  mb_err e = mb_store_next_stamp(s, &seq, &lamport);
  if (e != MB_OK) { mb_store_rollback(s); return e; }

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO bill(id,number,counterparty_id,status,memo,currency,created_at,due_date,"
        "device_id,seq,lamport) VALUES(?,?,?,'DRAFT',?,?,?,?,"
        "(SELECT v FROM book_meta WHERE k='device_id'),?,?);", -1, &st, NULL) != SQLITE_OK) {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); mb_store_rollback(s); return e;
  }
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  if (number) sqlite3_bind_text(st, 2, number, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 2);
  sqlite3_bind_text(st, 3, counterparty_id, -1, SQLITE_TRANSIENT);
  if (memo) sqlite3_bind_text(st, 4, memo, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 4);
  sqlite3_bind_text(st, 5, currency, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, created, -1, SQLITE_TRANSIENT);
  if (due_date) sqlite3_bind_text(st, 7, due_date, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 7);
  sqlite3_bind_int64(st, 8, seq);
  sqlite3_bind_int64(st, 9, lamport);
  int rc = sqlite3_step(st);
  if (rc != SQLITE_DONE) {
    e = (sqlite3_extended_errcode(mb_store_handle(s)) == SQLITE_CONSTRAINT_FOREIGNKEY)
        ? MB_FAIL(MB_ERR_NOT_FOUND, "counterparty does not exist")
        : MB_FAIL(MB_ERR_DB, "insert bill: %s", sqlite3_errmsg(mb_store_handle(s)));
    sqlite3_finalize(st); mb_store_rollback(s); return e;
  }
  sqlite3_finalize(st);
  MB_TRY(mb_store_commit(s));
  snprintf(id_out, 40, "%s", id);
  return MB_OK;
}

static mb_err bill_status(mb_store *s, const char *id, mb_bill_status *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT status FROM bill WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  mb_err e;
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW)       { *out = parse_status((const char *)sqlite3_column_text(st, 0)); e = MB_OK; }
  else if (rc == SQLITE_DONE) { e = MB_FAIL(MB_ERR_NOT_FOUND, "bill '%s'", id); }
  else                        { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  sqlite3_finalize(st);
  return e;
}

mb_err mb_bill_add_line(mb_store *s, const char *bill_id, const mb_bill_line_in *in, char line_id_out[40]) {
  if (!in || !in->description || !in->description[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "line description required");
  if (!in->account_id || !in->account_id[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "line account required");
  mb_bill_status st_now;
  MB_TRY(bill_status(s, bill_id, &st_now));
  if (st_now != MB_BILL_DRAFT) return MB_FAIL(MB_ERR_INVALID_ARG, "can only edit a DRAFT bill");

  int64_t qty = in->qty_centi > 0 ? in->qty_centi : 100;
  mb_money total;
  MB_TRY(mb_money_line_total(in->unit_price, qty, &total));

  char lid[40];
  MB_TRY(mb_uuid(lid, sizeof lid));
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO bill_line(id,bill_id,item_id,description,qty_centi,unit_price,line_total,"
        "account_id,is_tax) VALUES(?,?,?,?,?,?,?,?,?);", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, lid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, bill_id, -1, SQLITE_TRANSIENT);
  if (in->item_id) sqlite3_bind_text(st, 3, in->item_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 3);
  sqlite3_bind_text(st, 4, in->description, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, qty);
  sqlite3_bind_int64(st, 6, in->unit_price);
  sqlite3_bind_int64(st, 7, total);
  sqlite3_bind_text(st, 8, in->account_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 9, in->is_tax ? 1 : 0);
  int rc = sqlite3_step(st);
  mb_err e;
  if (rc == SQLITE_DONE) e = MB_OK;
  else e = (sqlite3_extended_errcode(mb_store_handle(s)) == SQLITE_CONSTRAINT_FOREIGNKEY)
           ? MB_FAIL(MB_ERR_NOT_FOUND, "bill or account does not exist")
           : MB_FAIL(MB_ERR_DB, "insert line: %s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  if (e == MB_OK) snprintf(line_id_out, 40, "%s", lid);
  return e;
}

mb_err mb_bill_total(mb_store *s, const char *bill_id, mb_money *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT COALESCE(SUM(line_total),0) FROM bill_line WHERE bill_id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, bill_id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_ROW) ? (*out = (mb_money)sqlite3_column_int64(st, 0), MB_OK)
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_bill_enter(mb_store *s, const char *bill_id, const char *issue_date) {
  if (!issue_date || !issue_date[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "issue_date required");
  mb_bill_status st_now;
  MB_TRY(bill_status(s, bill_id, &st_now));
  if (st_now != MB_BILL_DRAFT) return MB_FAIL(MB_ERR_INVALID_ARG, "bill is not a DRAFT");

  char ap[40];
  if (mb_store_meta_get(s, "ap_account_id", ap, sizeof ap) != MB_OK)
    return MB_FAIL(MB_ERR_INTERNAL, "no Accounts Payable account configured (seed the book)");

  /* counterparty for the AP posting (D26: per-vendor balance) */
  char cp[40] = "";
  {
    sqlite3_stmt *cst;
    if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT counterparty_id FROM bill WHERE id=?;", -1, &cst, NULL) != SQLITE_OK)
      return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
    sqlite3_bind_text(cst, 1, bill_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(cst) == SQLITE_ROW) {
      const char *c = (const char *)sqlite3_column_text(cst, 0);
      snprintf(cp, sizeof cp, "%s", c ? c : "");
    }
    sqlite3_finalize(cst);
  }

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT account_id, line_total FROM bill_line WHERE bill_id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, bill_id, -1, SQLITE_TRANSIENT);

  int cap = 8, n = 0;
  char (*acct)[40] = malloc((size_t)cap * sizeof *acct);
  mb_money *amt = malloc((size_t)cap * sizeof *amt);
  if (!acct || !amt) { free(acct); free(amt); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  mb_money total = 0;
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (n == cap) {
      cap *= 2;
      char (*na)[40] = realloc(acct, (size_t)cap * sizeof *na);
      mb_money *nm = realloc(amt, (size_t)cap * sizeof *nm);
      if (!na || !nm) { free(na ? (void*)na : (void*)acct); free(nm ? nm : amt); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      acct = na; amt = nm;
    }
    snprintf(acct[n], 40, "%s", (const char *)sqlite3_column_text(st, 0));
    amt[n] = (mb_money)sqlite3_column_int64(st, 1);
    total += amt[n];
    n++;
  }
  sqlite3_finalize(st);
  if (n == 0) { free(acct); free(amt); return MB_FAIL(MB_ERR_INVALID_ARG, "bill has no lines"); }

  /* postings: Dr each expense line; Cr AP total */
  mb_posting_in *p = malloc((size_t)(n + 1) * sizeof *p);
  if (!p) { free(acct); free(amt); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  p[0].account_id = ap; p[0].amount = -total; p[0].memo = NULL; p[0].counterparty_id = cp[0] ? cp : NULL;
  for (int i = 0; i < n; i++) { p[i+1].account_id = acct[i]; p[i+1].amount = amt[i]; p[i+1].memo = NULL; p[i+1].counterparty_id = NULL; }

  char memo[64];
  snprintf(memo, sizeof memo, "Bill %.40s", bill_id);

  mb_err e = mb_store_begin(s);
  if (e == MB_OK) {
    char entry_id[40];
    e = mb_journal_post_tx(s, issue_date, memo, MB_SRC_USER, p, n + 1, entry_id);
    if (e == MB_OK) {
      sqlite3_stmt *u;
      if (sqlite3_prepare_v2(mb_store_handle(s),
            "UPDATE bill SET status='OPEN', issue_date=?, entry_id=? WHERE id=?;", -1, &u, NULL) == SQLITE_OK) {
        sqlite3_bind_text(u, 1, issue_date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u, 2, entry_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u, 3, bill_id, -1, SQLITE_TRANSIENT);
        e = (sqlite3_step(u) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "update bill: %s", sqlite3_errmsg(mb_store_handle(s)));
        sqlite3_finalize(u);
      } else e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
    }
    if (e != MB_OK) mb_store_rollback(s); else e = mb_store_commit(s);
  }
  free(p); free(acct); free(amt);
  return e;
}

mb_err mb_bill_get(mb_store *s, const char *id, mb_bill *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT id,number,counterparty_id,issue_date,due_date,status,memo,currency,entry_id "
        "FROM bill WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  mb_err e;
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    memset(out, 0, sizeof *out);
    const char *c;
    snprintf(out->id, sizeof out->id, "%s", (const char *)sqlite3_column_text(st, 0));
    c = (const char *)sqlite3_column_text(st, 1); snprintf(out->number, sizeof out->number, "%s", c ? c : "");
    snprintf(out->counterparty_id, sizeof out->counterparty_id, "%s", (const char *)sqlite3_column_text(st, 2));
    c = (const char *)sqlite3_column_text(st, 3); snprintf(out->issue_date, sizeof out->issue_date, "%s", c ? c : "");
    c = (const char *)sqlite3_column_text(st, 4); snprintf(out->due_date, sizeof out->due_date, "%s", c ? c : "");
    out->status = parse_status((const char *)sqlite3_column_text(st, 5));
    c = (const char *)sqlite3_column_text(st, 6); snprintf(out->memo, sizeof out->memo, "%s", c ? c : "");
    snprintf(out->currency, sizeof out->currency, "%s", (const char *)sqlite3_column_text(st, 7));
    c = (const char *)sqlite3_column_text(st, 8); snprintf(out->entry_id, sizeof out->entry_id, "%s", c ? c : "");
    e = MB_OK;
  } else if (rc == SQLITE_DONE) {
    e = MB_FAIL(MB_ERR_NOT_FOUND, "bill '%s'", id);
  } else {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  }
  sqlite3_finalize(st);
  return e;
}

mb_err mb_bill_list(mb_store *s, mb_bill_row **rows_out, int *n_out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT b.id, b.number, b.counterparty_id, c.name, b.issue_date, b.due_date, b.status,"
        "  (SELECT COALESCE(SUM(line_total),0) FROM bill_line WHERE bill_id=b.id) "
        "FROM bill b JOIN counterparty c ON c.id=b.counterparty_id "
        "ORDER BY b.created_at DESC, b.id DESC;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  int cap = 16, cnt = 0;
  mb_bill_row *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (cnt == cap) {
      cap *= 2;
      mb_bill_row *na = realloc(arr, (size_t)cap * sizeof *na);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    mb_bill_row *r = &arr[cnt++];
    memset(r, 0, sizeof *r);
    const char *c;
    snprintf(r->id, sizeof r->id, "%s", (const char *)sqlite3_column_text(st, 0));
    c = (const char *)sqlite3_column_text(st, 1); snprintf(r->number, sizeof r->number, "%s", c ? c : "");
    snprintf(r->counterparty_id, sizeof r->counterparty_id, "%s", (const char *)sqlite3_column_text(st, 2));
    c = (const char *)sqlite3_column_text(st, 3); snprintf(r->counterparty_name, sizeof r->counterparty_name, "%s", c ? c : "");
    c = (const char *)sqlite3_column_text(st, 4); snprintf(r->issue_date, sizeof r->issue_date, "%s", c ? c : "");
    c = (const char *)sqlite3_column_text(st, 5); snprintf(r->due_date, sizeof r->due_date, "%s", c ? c : "");
    r->status = parse_status((const char *)sqlite3_column_text(st, 6));
    r->total = (mb_money)sqlite3_column_int64(st, 7);
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) { free(arr); return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  *rows_out = arr;
  *n_out = cnt;
  return MB_OK;
}

mb_err mb_bill_lines(mb_store *s, const char *bill_id, mb_bill_line **rows, int *n) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT l.id, l.description, l.qty_centi, l.unit_price, l.line_total, l.account_id, "
        "       a.name, l.is_tax "
        "FROM bill_line l JOIN account a ON a.id=l.account_id "
        "WHERE l.bill_id=? ORDER BY l.rowid;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, bill_id, -1, SQLITE_TRANSIENT);
  int cap = 8, cnt = 0;
  mb_bill_line *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (cnt == cap) {
      cap *= 2;
      mb_bill_line *na = realloc(arr, (size_t)cap * sizeof *na);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    mb_bill_line *r = &arr[cnt++];
    memset(r, 0, sizeof *r);
    const char *c;
    snprintf(r->id, sizeof r->id, "%s", (const char *)sqlite3_column_text(st, 0));
    c = (const char *)sqlite3_column_text(st, 1); snprintf(r->description, sizeof r->description, "%s", c ? c : "");
    r->qty_centi = sqlite3_column_int64(st, 2);
    r->unit_price = (mb_money)sqlite3_column_int64(st, 3);
    r->line_total = (mb_money)sqlite3_column_int64(st, 4);
    snprintf(r->account_id, sizeof r->account_id, "%s", (const char *)sqlite3_column_text(st, 5));
    c = (const char *)sqlite3_column_text(st, 6); snprintf(r->account_name, sizeof r->account_name, "%s", c ? c : "");
    r->is_tax = sqlite3_column_int(st, 7);
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) { free(arr); return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  *rows = arr;
  *n = cnt;
  return MB_OK;
}

mb_err mb_bill_remove_line(mb_store *s, const char *line_id) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT b.status FROM bill_line l JOIN bill b ON b.id=l.bill_id WHERE l.id=?;",
        -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, line_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_NOT_FOUND, "line '%s'", line_id); }
  mb_bill_status st_now = parse_status((const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  if (st_now != MB_BILL_DRAFT) return MB_FAIL(MB_ERR_INVALID_ARG, "can only edit a DRAFT bill");

  if (sqlite3_prepare_v2(mb_store_handle(s), "DELETE FROM bill_line WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, line_id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_bill_update(mb_store *s, const char *id, const char *number,
                      const char *due_date, const char *memo) {
  mb_bill_status st_now;
  MB_TRY(bill_status(s, id, &st_now));
  if (st_now != MB_BILL_DRAFT) return MB_FAIL(MB_ERR_INVALID_ARG, "can only edit a DRAFT bill");
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "UPDATE bill SET number=?, due_date=?, memo=? WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, number, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, due_date, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, memo, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_bill_void(mb_store *s, const char *id) {
  mb_bill b;
  MB_TRY(mb_bill_get(s, id, &b));
  if (b.status == MB_BILL_DRAFT)
    return MB_FAIL(MB_ERR_INVALID_ARG, "a draft bill is not entered — edit or discard it instead");
  if (b.status == MB_BILL_VOID)
    return MB_FAIL(MB_ERR_INVALID_ARG, "bill is already void");
  if (b.status != MB_BILL_OPEN)
    return MB_FAIL(MB_ERR_INVALID_ARG, "a paid or partly-paid bill cannot be voided — issue a refund or credit note");
  if (!b.entry_id[0]) return MB_FAIL(MB_ERR_INTERNAL, "entered bill has no journal entry");

  /* Reverse the entry (cancels AP + expense; flags the original REVERSED), then mark VOID. */
  char rev[40];
  MB_TRY(mb_journal_reverse(s, b.entry_id, rev));
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "UPDATE bill SET status='VOID' WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../account/account.h"
#include "../counterparty/counterparty.h"
#include "../journal/journal.h"
#include "../payment/payment.h"
#include "../report/report.h"
#include "../seed/seed.h"

TEST(bill, enter_posts_expense_and_ap) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char expense[40];
  mb_account_new acc = {.code="6010", .name="Software", .type=MB_ACCT_EXPENSE, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &acc, expense));
  char vendor[40];
  mb_counterparty_new v = {.name="AWS", .kind=MB_CP_VENDOR};
  ASSERT_OK(mb_counterparty_create(s, &v, vendor));

  char bill[40];
  ASSERT_OK(mb_bill_create(s, vendor, "AWS-07", "2026-07-15", NULL, bill));
  char lid[40];
  mb_bill_line_in l = {.description="Hosting", .qty_centi=100, .unit_price=12000, .account_id=expense};
  ASSERT_OK(mb_bill_add_line(s, bill, &l, lid));
  mb_money total; ASSERT_OK(mb_bill_total(s, bill, &total)); ASSERT_MONEY_EQ(total, 12000);

  ASSERT_OK(mb_bill_enter(s, bill, "2026-07-15"));
  mb_bill got; ASSERT_OK(mb_bill_get(s, bill, &got));
  ASSERT_EQ_INT(got.status, MB_BILL_OPEN);

  char ap[40]; ASSERT_OK(mb_store_meta_get(s, "ap_account_id", ap, sizeof ap));
  mb_money bal;
  ASSERT_OK(mb_account_balance(s, expense, &bal)); ASSERT_MONEY_EQ(bal, 12000);   /* Dr Expense */
  ASSERT_OK(mb_account_balance(s, ap, &bal));      ASSERT_MONEY_EQ(bal, -12000);  /* Cr AP */
  mb_store_close(s);
}

TEST(bill, list_carries_vendor_status_total) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char expense[40];
  mb_account_new acc = {.code="6010", .name="Software", .type=MB_ACCT_EXPENSE, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &acc, expense));
  char vendor[40];
  mb_counterparty_new v = {.name="AWS", .kind=MB_CP_VENDOR};
  ASSERT_OK(mb_counterparty_create(s, &v, vendor));
  char bill[40]; ASSERT_OK(mb_bill_create(s, vendor, "AWS-07", "2026-07-15", NULL, bill));
  char lid[40];
  mb_bill_line_in l = {.description="Hosting", .qty_centi=100, .unit_price=12000, .account_id=expense};
  ASSERT_OK(mb_bill_add_line(s, bill, &l, lid));

  mb_bill_row *rows = NULL; int n = 0;
  ASSERT_OK(mb_bill_list(s, &rows, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_STR_EQ(rows[0].number, "AWS-07");
  ASSERT_STR_EQ(rows[0].counterparty_name, "AWS");
  ASSERT_EQ_INT(rows[0].status, MB_BILL_DRAFT);
  ASSERT_MONEY_EQ(rows[0].total, 12000);
  free(rows);
  mb_store_close(s);
}

/* ---- bill edit/lock/void family — mirror of the invoice tests (audit F3) ---- */

/* set up a seeded book with one expense category + one vendor */
static void bill_setup(mb_test *t, mb_store *s, char expense[40], char vendor[40]) {
  ASSERT_OK(mb_seed_system_accounts(s));
  mb_account_new acc = {.code="6010", .name="Software", .type=MB_ACCT_EXPENSE, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &acc, expense));
  mb_counterparty_new v = {.name="AWS", .kind=MB_CP_VENDOR};
  ASSERT_OK(mb_counterparty_create(s, &v, vendor));
}

/* D13: once a bill is entered it is locked — no line edits, no re-enter (twin of
 * invoice.cannot_edit_after_issue, plus the remove_line/update paths). */
TEST(bill, cannot_edit_after_enter) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char expense[40], vendor[40]; bill_setup(t, s, expense, vendor);
  char bill[40]; ASSERT_OK(mb_bill_create(s, vendor, NULL, NULL, NULL, bill));
  char lid[40];
  mb_bill_line_in l = {.description="X", .qty_centi=100, .unit_price=5000, .account_id=expense};
  ASSERT_OK(mb_bill_add_line(s, bill, &l, lid));
  ASSERT_OK(mb_bill_enter(s, bill, "2026-07-03"));
  /* now locked */
  ASSERT_ERR(mb_bill_add_line(s, bill, &l, lid), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_bill_enter(s, bill, "2026-07-03"), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_bill_remove_line(s, lid), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_bill_update(s, bill, "B-9", "2026-08-01", "no"), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}

/* entering a bill with no lines fails (twin of invoice.issue_empty_fails) */
TEST(bill, enter_empty_fails) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char expense[40], vendor[40]; bill_setup(t, s, expense, vendor);
  char bill[40]; ASSERT_OK(mb_bill_create(s, vendor, NULL, NULL, NULL, bill));
  ASSERT_ERR(mb_bill_enter(s, bill, "2026-07-04"), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}

/* edit a DRAFT (remove_line + lines + update), enter (AP credited + locked), then void (reverse →
 * AP nets to zero per vendor). The AP twin of the invoice edit/lock/void tests. */
TEST(bill, edit_draft_then_lock_and_void) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char expense[40], vendor[40]; bill_setup(t, s, expense, vendor);
  char ap[40]; ASSERT_OK(mb_store_meta_get(s, "ap_account_id", ap, sizeof ap));

  char bill[40]; ASSERT_OK(mb_bill_create(s, vendor, "B-1", "2026-07-01", NULL, bill));
  char l1[40], l2[40];
  mb_bill_line_in a = {.description="A", .qty_centi=100, .unit_price=10000, .account_id=expense};
  mb_bill_line_in b = {.description="B", .qty_centi=100, .unit_price=5000, .account_id=expense};
  ASSERT_OK(mb_bill_add_line(s, bill, &a, l1));
  ASSERT_OK(mb_bill_add_line(s, bill, &b, l2));

  /* edit a DRAFT: remove a line, read lines back, update header */
  ASSERT_OK(mb_bill_remove_line(s, l2));
  mb_bill_line *lines = NULL; int ln = 0;
  ASSERT_OK(mb_bill_lines(s, bill, &lines, &ln));
  ASSERT_EQ_INT(ln, 1);
  ASSERT_STR_EQ(lines[0].description, "A");
  ASSERT_STR_EQ(lines[0].account_name, "Software");
  free(lines);
  ASSERT_OK(mb_bill_update(s, bill, "B-1b", "2026-08-01", "edited"));

  /* enter → OPEN, AP credited (credit-normal control → negative debit-sum), then locked */
  ASSERT_OK(mb_bill_enter(s, bill, "2026-07-01"));
  mb_money bal; ASSERT_OK(mb_account_balance(s, ap, &bal)); ASSERT_MONEY_EQ(bal, -10000);
  mb_money vbal; ASSERT_OK(mb_counterparty_balance(s, vendor, MB_PAY_BILL, &vbal));
  ASSERT_MONEY_EQ(vbal, -10000);   /* we owe the vendor $100 */
  ASSERT_ERR(mb_bill_remove_line(s, l1), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_bill_update(s, bill, "X", NULL, NULL), MB_ERR_INVALID_ARG);

  /* void the OPEN bill → reversing entry cancels AP, status VOID, AP nets to zero per vendor */
  ASSERT_OK(mb_bill_void(s, bill));
  mb_bill got; ASSERT_OK(mb_bill_get(s, bill, &got));
  ASSERT_EQ_INT(got.status, MB_BILL_VOID);
  ASSERT_OK(mb_account_balance(s, ap, &bal)); ASSERT_MONEY_EQ(bal, 0);
  ASSERT_OK(mb_counterparty_balance(s, vendor, MB_PAY_BILL, &vbal));
  ASSERT_MONEY_EQ(vbal, 0);   /* reversal preserved the vendor tag, so per-vendor AP nets to zero too */
  mb_aging ag; ASSERT_OK(mb_report_ap_aging(s, "2026-12-31", &ag)); ASSERT_MONEY_EQ(ag.total, 0);

  /* terminal: cannot void again, and a draft cannot be voided */
  ASSERT_ERR(mb_bill_void(s, bill), MB_ERR_INVALID_ARG);
  char d[40]; ASSERT_OK(mb_bill_create(s, vendor, NULL, NULL, NULL, d));
  ASSERT_ERR(mb_bill_void(s, d), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}
#endif
