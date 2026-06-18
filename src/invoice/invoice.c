#include "invoice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../support/mb_id.h"
#include "../journal/journal.h"

const char *mb_invoice_status_str(mb_invoice_status st) {
  switch (st) {
    case MB_INV_DRAFT:   return "DRAFT";
    case MB_INV_OPEN:    return "OPEN";
    case MB_INV_PARTIAL: return "PARTIAL";
    case MB_INV_PAID:    return "PAID";
    case MB_INV_VOID:    return "VOID";
  }
  return "DRAFT";
}

static mb_invoice_status parse_status(const char *s) {
  if (!strcmp(s, "OPEN"))    return MB_INV_OPEN;
  if (!strcmp(s, "PARTIAL")) return MB_INV_PARTIAL;
  if (!strcmp(s, "PAID"))    return MB_INV_PAID;
  if (!strcmp(s, "VOID"))    return MB_INV_VOID;
  return MB_INV_DRAFT;
}

mb_err mb_invoice_create(mb_store *s, const char *counterparty_id, const char *number,
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
        "INSERT INTO invoice(id,number,counterparty_id,status,memo,currency,created_at,"
        "due_date,device_id,seq,lamport) VALUES(?,?,?,'DRAFT',?,?,?,?,"
        "(SELECT v FROM book_meta WHERE k='device_id'),?,?);", -1, &st, NULL) != SQLITE_OK) {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
    mb_store_rollback(s);
    return e;
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
        : MB_FAIL(MB_ERR_DB, "insert invoice: %s", sqlite3_errmsg(mb_store_handle(s)));
    sqlite3_finalize(st); mb_store_rollback(s); return e;
  }
  sqlite3_finalize(st);
  MB_TRY(mb_store_commit(s));
  snprintf(id_out, 40, "%s", id);
  return MB_OK;
}

static mb_err invoice_status(mb_store *s, const char *id, mb_invoice_status *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT status FROM invoice WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  mb_err e;
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW)       { *out = parse_status((const char *)sqlite3_column_text(st, 0)); e = MB_OK; }
  else if (rc == SQLITE_DONE) { e = MB_FAIL(MB_ERR_NOT_FOUND, "invoice '%s'", id); }
  else                        { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  sqlite3_finalize(st);
  return e;
}

mb_err mb_invoice_add_line(mb_store *s, const char *invoice_id, const mb_invoice_line_in *in,
                           char line_id_out[40]) {
  if (!in || !in->description || !in->description[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "line description required");
  if (!in->account_id || !in->account_id[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "line account required");

  mb_invoice_status st_now;
  MB_TRY(invoice_status(s, invoice_id, &st_now));
  if (st_now != MB_INV_DRAFT) return MB_FAIL(MB_ERR_INVALID_ARG, "can only edit a DRAFT invoice");

  int64_t qty = in->qty_centi > 0 ? in->qty_centi : 100;
  mb_money total;
  MB_TRY(mb_money_line_total(in->unit_price, qty, &total));

  char lid[40];
  MB_TRY(mb_uuid(lid, sizeof lid));
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO invoice_line(id,invoice_id,item_id,description,qty_centi,unit_price,"
        "line_total,account_id,is_tax) VALUES(?,?,?,?,?,?,?,?,?);", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, lid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, invoice_id, -1, SQLITE_TRANSIENT);
  if (in->item_id) sqlite3_bind_text(st, 3, in->item_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 3);
  sqlite3_bind_text(st, 4, in->description, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, qty);
  sqlite3_bind_int64(st, 6, in->unit_price);
  sqlite3_bind_int64(st, 7, total);
  sqlite3_bind_text(st, 8, in->account_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 9, in->is_tax ? 1 : 0);

  int rc = sqlite3_step(st);
  mb_err e;
  if (rc == SQLITE_DONE) { e = MB_OK; }
  else {
    e = (sqlite3_extended_errcode(mb_store_handle(s)) == SQLITE_CONSTRAINT_FOREIGNKEY)
        ? MB_FAIL(MB_ERR_NOT_FOUND, "invoice or account does not exist")
        : MB_FAIL(MB_ERR_DB, "insert line: %s", sqlite3_errmsg(mb_store_handle(s)));
  }
  sqlite3_finalize(st);
  if (e == MB_OK) snprintf(line_id_out, 40, "%s", lid);
  return e;
}

mb_err mb_invoice_total(mb_store *s, const char *invoice_id, mb_money *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT COALESCE(SUM(line_total),0) FROM invoice_line WHERE invoice_id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, invoice_id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_ROW) ? (*out = (mb_money)sqlite3_column_int64(st, 0), MB_OK)
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_invoice_issue(mb_store *s, const char *invoice_id, const char *issue_date) {
  if (!issue_date || !issue_date[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "issue_date required");

  mb_invoice_status st_now;
  MB_TRY(invoice_status(s, invoice_id, &st_now));
  if (st_now != MB_INV_DRAFT) return MB_FAIL(MB_ERR_INVALID_ARG, "invoice is not a DRAFT");

  char ar[40];
  if (mb_store_meta_get(s, "ar_account_id", ar, sizeof ar) != MB_OK)
    return MB_FAIL(MB_ERR_INTERNAL, "no Accounts Receivable account configured (seed the book)");

  /* counterparty for the AR posting (D26: per-customer balance) */
  char cp[40] = "";
  {
    sqlite3_stmt *cst;
    if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT counterparty_id FROM invoice WHERE id=?;", -1, &cst, NULL) != SQLITE_OK)
      return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
    sqlite3_bind_text(cst, 1, invoice_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(cst) == SQLITE_ROW) {
      const char *c = (const char *)sqlite3_column_text(cst, 0);
      snprintf(cp, sizeof cp, "%s", c ? c : "");
    }
    sqlite3_finalize(cst);
  }

  /* gather lines */
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT account_id, line_total FROM invoice_line WHERE invoice_id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, invoice_id, -1, SQLITE_TRANSIENT);

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
  if (n == 0) { free(acct); free(amt); return MB_FAIL(MB_ERR_INVALID_ARG, "invoice has no lines"); }

  /* postings: Dr AR total; Cr each line account */
  mb_posting_in *p = malloc((size_t)(n + 1) * sizeof *p);
  if (!p) { free(acct); free(amt); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  p[0].account_id = ar; p[0].amount = total; p[0].memo = NULL; p[0].counterparty_id = cp[0] ? cp : NULL;
  for (int i = 0; i < n; i++) { p[i+1].account_id = acct[i]; p[i+1].amount = -amt[i]; p[i+1].memo = NULL; p[i+1].counterparty_id = NULL; }

  char memo[64];
  snprintf(memo, sizeof memo, "Invoice %.40s", invoice_id);

  mb_err e = mb_store_begin(s);
  if (e == MB_OK) {
    char entry_id[40];
    e = mb_journal_post_tx(s, issue_date, memo, MB_SRC_USER, p, n + 1, entry_id);
    if (e == MB_OK) {
      sqlite3_stmt *u;
      if (sqlite3_prepare_v2(mb_store_handle(s),
            "UPDATE invoice SET status='OPEN', issue_date=?, entry_id=? WHERE id=?;", -1, &u, NULL) == SQLITE_OK) {
        sqlite3_bind_text(u, 1, issue_date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u, 2, entry_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u, 3, invoice_id, -1, SQLITE_TRANSIENT);
        e = (sqlite3_step(u) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "update invoice: %s", sqlite3_errmsg(mb_store_handle(s)));
        sqlite3_finalize(u);
      } else {
        e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
      }
    }
    if (e != MB_OK) mb_store_rollback(s); else e = mb_store_commit(s);
  }
  free(p); free(acct); free(amt);
  return e;
}

mb_err mb_invoice_get(mb_store *s, const char *id, mb_invoice *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT id,number,counterparty_id,issue_date,due_date,status,memo,currency,entry_id "
        "FROM invoice WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
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
    e = MB_FAIL(MB_ERR_NOT_FOUND, "invoice '%s'", id);
  } else {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  }
  sqlite3_finalize(st);
  return e;
}

mb_err mb_invoice_list(mb_store *s, mb_invoice_row **rows_out, int *n_out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT i.id, i.number, i.counterparty_id, c.name, i.issue_date, i.due_date, i.status,"
        "  (SELECT COALESCE(SUM(line_total),0) FROM invoice_line WHERE invoice_id=i.id) "
        "FROM invoice i JOIN counterparty c ON c.id=i.counterparty_id "
        "ORDER BY i.created_at DESC, i.id DESC;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  int cap = 16, cnt = 0;
  mb_invoice_row *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (cnt == cap) {
      cap *= 2;
      mb_invoice_row *na = realloc(arr, (size_t)cap * sizeof *na);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    mb_invoice_row *r = &arr[cnt++];
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

mb_err mb_invoice_lines(mb_store *s, const char *invoice_id, mb_invoice_line **rows, int *n) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT l.id, l.description, l.qty_centi, l.unit_price, l.line_total, l.account_id, "
        "       a.name, l.is_tax "
        "FROM invoice_line l JOIN account a ON a.id=l.account_id "
        "WHERE l.invoice_id=? ORDER BY l.rowid;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, invoice_id, -1, SQLITE_TRANSIENT);
  int cap = 8, cnt = 0;
  mb_invoice_line *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (cnt == cap) {
      cap *= 2;
      mb_invoice_line *na = realloc(arr, (size_t)cap * sizeof *na);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    mb_invoice_line *r = &arr[cnt++];
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

mb_err mb_invoice_remove_line(mb_store *s, const char *line_id) {
  /* only on a DRAFT invoice */
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT i.status FROM invoice_line l JOIN invoice i ON i.id=l.invoice_id WHERE l.id=?;",
        -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, line_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_NOT_FOUND, "line '%s'", line_id); }
  mb_invoice_status st_now = parse_status((const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  if (st_now != MB_INV_DRAFT) return MB_FAIL(MB_ERR_INVALID_ARG, "can only edit a DRAFT invoice");

  if (sqlite3_prepare_v2(mb_store_handle(s), "DELETE FROM invoice_line WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, line_id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_invoice_update(mb_store *s, const char *id, const char *number,
                         const char *due_date, const char *memo) {
  mb_invoice_status st_now;
  MB_TRY(invoice_status(s, id, &st_now));
  if (st_now != MB_INV_DRAFT) return MB_FAIL(MB_ERR_INVALID_ARG, "can only edit a DRAFT invoice");
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "UPDATE invoice SET number=?, due_date=?, memo=? WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, number, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, due_date, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, memo, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_invoice_void(mb_store *s, const char *id) {
  mb_invoice inv;
  MB_TRY(mb_invoice_get(s, id, &inv));
  /* Only an issued, unpaid invoice can be voided. A DRAFT was never posted; a PARTIAL/PAID one has
   * cash applied — correct those with a refund or a credit note, not a void. */
  if (inv.status == MB_INV_DRAFT)
    return MB_FAIL(MB_ERR_INVALID_ARG, "a draft invoice is not issued — edit or discard it instead");
  if (inv.status == MB_INV_VOID)
    return MB_FAIL(MB_ERR_INVALID_ARG, "invoice is already void");
  if (inv.status != MB_INV_OPEN)
    return MB_FAIL(MB_ERR_INVALID_ARG, "a paid or partly-paid invoice cannot be voided — issue a refund or credit note");
  if (!inv.entry_id[0]) return MB_FAIL(MB_ERR_INTERNAL, "issued invoice has no journal entry");

  /* Reverse the issue posting (cancels AR + income; the reversal flags the original REVERSED),
   * then mark the document VOID — it leaves AR aging and the effective reports. */
  char rev[40];
  MB_TRY(mb_journal_reverse(s, inv.entry_id, rev));
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "UPDATE invoice SET status='VOID' WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
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
#include "../payment/payment.h"
#include "../report/report.h"
#include "../seed/seed.h"

TEST(invoice, draft_then_issue_posts) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));   /* AR + tax payable */
  char income[40];
  mb_account_new acc = {.code="4000", .name="Consulting Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &acc, income));
  char cp[40];
  mb_counterparty_new cpn = {.name="Acme", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &cpn, cp));

  char inv[40];
  ASSERT_OK(mb_invoice_create(s, cp, "INV-001", "2026-07-01", "July work", inv));
  char l1[40], l2[40];
  mb_invoice_line_in a = {.description="Consulting", .qty_centi=1000, .unit_price=15000, .account_id=income};
  mb_invoice_line_in b = {.description="Extra hour", .qty_centi=100,  .unit_price=15000, .account_id=income};
  ASSERT_OK(mb_invoice_add_line(s, inv, &a, l1));   /* 10.0 x 150 = 1500.00 */
  ASSERT_OK(mb_invoice_add_line(s, inv, &b, l2));   /* 1 x 150  =  150.00 */
  mb_money total; ASSERT_OK(mb_invoice_total(s, inv, &total));
  ASSERT_MONEY_EQ(total, 165000);

  ASSERT_OK(mb_invoice_issue(s, inv, "2026-07-01"));
  mb_invoice got; ASSERT_OK(mb_invoice_get(s, inv, &got));
  ASSERT_EQ_INT(got.status, MB_INV_OPEN);
  ASSERT(got.entry_id[0] != '\0');

  char ar[40]; ASSERT_OK(mb_store_meta_get(s, "ar_account_id", ar, sizeof ar));
  mb_money bal;
  ASSERT_OK(mb_account_balance(s, ar, &bal));      ASSERT_MONEY_EQ(bal, 165000);   /* Dr AR */
  ASSERT_OK(mb_account_balance(s, income, &bal));  ASSERT_MONEY_EQ(bal, -165000);  /* Cr Income */
  mb_store_close(s);
}

TEST(invoice, tax_line_hits_liability) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40], tax[40];
  mb_account_new acc = {.code="4000", .name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &acc, income));
  ASSERT_OK(mb_store_meta_get(s, "tax_account_id", tax, sizeof tax));
  char cp[40]; mb_counterparty_new cpn = {.name="Client", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &cpn, cp));

  char inv[40]; ASSERT_OK(mb_invoice_create(s, cp, "INV-002", NULL, NULL, inv));
  char lid[40];
  mb_invoice_line_in svc = {.description="Service", .qty_centi=100, .unit_price=100000, .account_id=income};
  mb_invoice_line_in tl  = {.description="Sales tax", .qty_centi=100, .unit_price=8000, .account_id=tax, .is_tax=1};
  ASSERT_OK(mb_invoice_add_line(s, inv, &svc, lid));
  ASSERT_OK(mb_invoice_add_line(s, inv, &tl, lid));
  ASSERT_OK(mb_invoice_issue(s, inv, "2026-07-02"));

  mb_money bal;
  ASSERT_OK(mb_account_balance(s, tax, &bal));    ASSERT_MONEY_EQ(bal, -8000);   /* Cr Tax Payable */
  ASSERT_OK(mb_account_balance(s, income, &bal)); ASSERT_MONEY_EQ(bal, -100000);
  mb_store_close(s);
}

TEST(invoice, cannot_edit_after_issue) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40];
  mb_account_new acc = {.code="4000", .name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &acc, income));
  char cp[40]; mb_counterparty_new cpn = {.name="C", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &cpn, cp));
  char inv[40]; ASSERT_OK(mb_invoice_create(s, cp, NULL, NULL, NULL, inv));
  char lid[40];
  mb_invoice_line_in a = {.description="X", .qty_centi=100, .unit_price=5000, .account_id=income};
  ASSERT_OK(mb_invoice_add_line(s, inv, &a, lid));
  ASSERT_OK(mb_invoice_issue(s, inv, "2026-07-03"));
  /* now locked */
  ASSERT_ERR(mb_invoice_add_line(s, inv, &a, lid), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_invoice_issue(s, inv, "2026-07-03"), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}

TEST(invoice, issue_empty_fails) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char cp[40]; mb_counterparty_new cpn = {.name="C", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &cpn, cp));
  char inv[40]; ASSERT_OK(mb_invoice_create(s, cp, NULL, NULL, NULL, inv));
  ASSERT_ERR(mb_invoice_issue(s, inv, "2026-07-04"), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}

TEST(invoice, list_carries_name_status_total) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40];
  mb_account_new acc = {.code="4000", .name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &acc, income));
  char cp[40]; mb_counterparty_new cpn = {.name="Acme Co", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &cpn, cp));

  char inv[40]; ASSERT_OK(mb_invoice_create(s, cp, "INV-100", "2026-07-01", NULL, inv));
  char lid[40];
  mb_invoice_line_in ln = {.description="Work", .qty_centi=200, .unit_price=10000, .account_id=income};
  ASSERT_OK(mb_invoice_add_line(s, inv, &ln, lid));   /* 2 x 100.00 = 200.00 */

  mb_invoice_row *rows = NULL; int n = 0;
  ASSERT_OK(mb_invoice_list(s, &rows, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_STR_EQ(rows[0].number, "INV-100");
  ASSERT_STR_EQ(rows[0].counterparty_name, "Acme Co");
  ASSERT_EQ_INT(rows[0].status, MB_INV_DRAFT);
  ASSERT_MONEY_EQ(rows[0].total, 20000);
  free(rows);
  mb_store_close(s);
}

TEST(invoice, edit_draft_then_lock_on_issue) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40];
  mb_account_new acc = {.code="4000", .name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &acc, income));
  char cp[40]; mb_counterparty_new cpn = {.name="C", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &cpn, cp));
  char inv[40]; ASSERT_OK(mb_invoice_create(s, cp, "INV-1", "2026-07-01", NULL, inv));
  char l1[40], l2[40];
  mb_invoice_line_in a = {.description="A", .qty_centi=100, .unit_price=10000, .account_id=income};
  mb_invoice_line_in b = {.description="B", .qty_centi=100, .unit_price=5000, .account_id=income};
  ASSERT_OK(mb_invoice_add_line(s, inv, &a, l1));
  ASSERT_OK(mb_invoice_add_line(s, inv, &b, l2));

  /* edit a DRAFT freely: remove a line, read lines back, update header */
  ASSERT_OK(mb_invoice_remove_line(s, l2));
  mb_invoice_line *lines = NULL; int ln = 0;
  ASSERT_OK(mb_invoice_lines(s, inv, &lines, &ln));
  ASSERT_EQ_INT(ln, 1);
  ASSERT_STR_EQ(lines[0].description, "A");
  ASSERT_STR_EQ(lines[0].account_name, "Income");
  free(lines);
  ASSERT_OK(mb_invoice_update(s, inv, "INV-1b", "2026-08-01", "edited"));

  /* issue → OPEN, AR debited, and now permanently locked (no reopen in the lifecycle) */
  ASSERT_OK(mb_invoice_issue(s, inv, "2026-07-01"));
  char ar[40]; ASSERT_OK(mb_store_meta_get(s, "ar_account_id", ar, sizeof ar));
  mb_money bal; ASSERT_OK(mb_account_balance(s, ar, &bal)); ASSERT_MONEY_EQ(bal, 10000);
  ASSERT_ERR(mb_invoice_remove_line(s, l1), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_invoice_add_line(s, inv, &a, l2), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_invoice_update(s, inv, "X", NULL, NULL), MB_ERR_INVALID_ARG);
  ASSERT_ERR(mb_invoice_issue(s, inv, "2026-07-02"), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}

/* Voiding an issued, unpaid invoice reverses its posting, marks it VOID, and removes it from AR +
 * aging + the effective income reports. DRAFT and paid/partial invoices cannot be voided. */
TEST(invoice, void_reverses_and_locks) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40], bank[40];
  mb_account_new acc = {.code="4000", .name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  mb_account_new ab  = {.code="1000", .name="Bank",   .type=MB_ACCT_ASSET,  .role=MB_ROLE_ACCOUNT};
  ASSERT_OK(mb_account_create(s, &acc, income));
  ASSERT_OK(mb_account_create(s, &ab, bank));
  char cp[40]; mb_counterparty_new cpn = {.name="C", .kind=MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &cpn, cp));
  char ar[40]; ASSERT_OK(mb_store_meta_get(s, "ar_account_id", ar, sizeof ar));

  /* a DRAFT cannot be voided (it was never issued) */
  char d[40]; ASSERT_OK(mb_invoice_create(s, cp, NULL, NULL, NULL, d));
  ASSERT_ERR(mb_invoice_void(s, d), MB_ERR_INVALID_ARG);

  /* issue a $100 invoice, then void it */
  char inv[40]; ASSERT_OK(mb_invoice_create(s, cp, "INV-1", NULL, NULL, inv));
  char lid[40];
  mb_invoice_line_in l = {.description="Work", .qty_centi=100, .unit_price=10000, .account_id=income};
  ASSERT_OK(mb_invoice_add_line(s, inv, &l, lid));
  ASSERT_OK(mb_invoice_issue(s, inv, "2026-07-01"));
  mb_money bal; ASSERT_OK(mb_account_balance(s, ar, &bal)); ASSERT_MONEY_EQ(bal, 10000);

  ASSERT_OK(mb_invoice_void(s, inv));
  mb_invoice got; ASSERT_OK(mb_invoice_get(s, inv, &got));
  ASSERT_EQ_INT(got.status, MB_INV_VOID);
  ASSERT_OK(mb_account_balance(s, ar, &bal)); ASSERT_MONEY_EQ(bal, 0);   /* AR cancelled */
  /* dropped from aging and effective income */
  mb_aging ag; ASSERT_OK(mb_report_ar_aging(s, "2026-12-31", &ag)); ASSERT_MONEY_EQ(ag.total, 0);
  mb_cat_txn_row *rows = NULL; int n = 0;
  ASSERT_OK(mb_report_category_txns(s, "INCOME", NULL, NULL, &rows, &n));
  ASSERT_EQ_INT(n, 0);
  free(rows);

  /* a void is terminal: cannot void again, cannot be paid */
  ASSERT_ERR(mb_invoice_void(s, inv), MB_ERR_INVALID_ARG);
  char pid[40];
  ASSERT_ERR(mb_payment_record(s, "2026-07-05", 10000, bank, MB_PAY_INVOICE, inv, pid), MB_ERR_INVALID_ARG);

  /* a paid invoice cannot be voided — use a refund/credit note */
  char inv2[40]; ASSERT_OK(mb_invoice_create(s, cp, "INV-2", NULL, NULL, inv2));
  ASSERT_OK(mb_invoice_add_line(s, inv2, &l, lid));
  ASSERT_OK(mb_invoice_issue(s, inv2, "2026-07-01"));
  ASSERT_OK(mb_payment_record(s, "2026-07-02", 10000, bank, MB_PAY_INVOICE, inv2, pid));  /* → PAID */
  ASSERT_ERR(mb_invoice_void(s, inv2), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}
#endif
