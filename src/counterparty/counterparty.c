#include "counterparty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../support/mb_id.h"

const char *mb_counterparty_kind_str(mb_counterparty_kind k) {
  switch (k) {
    case MB_CP_CUSTOMER: return "CUSTOMER";
    case MB_CP_VENDOR:   return "VENDOR";
    case MB_CP_BOTH:     return "BOTH";
  }
  return "CUSTOMER";
}

static mb_counterparty_kind parse_kind(const char *s) {
  if (!strcmp(s, "VENDOR")) return MB_CP_VENDOR;
  if (!strcmp(s, "BOTH"))   return MB_CP_BOTH;
  return MB_CP_CUSTOMER;
}

mb_err mb_counterparty_create(mb_store *s, const mb_counterparty_new *in, char id_out[40]) {
  if (!in || !in->name || !in->name[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "name required");

  char id[40], created[24];
  MB_TRY(mb_uuid(id, sizeof id));
  mb_now_iso(created, sizeof created);

  MB_TRY(mb_store_begin(s));
  int64_t seq, lamport;
  mb_err e = mb_store_next_stamp(s, &seq, &lamport);
  if (e != MB_OK) { mb_store_rollback(s); return e; }

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO counterparty(id,name,kind,email,phone,address,note,is_active,created_at,"
        "device_id,seq,lamport) VALUES(?,?,?,?,?,?,?,1,?,"
        "(SELECT v FROM book_meta WHERE k='device_id'),?,?);", -1, &st, NULL) != SQLITE_OK) {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
    mb_store_rollback(s);
    return e;
  }
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, in->name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, mb_counterparty_kind_str(in->kind), -1, SQLITE_TRANSIENT);
  if (in->email)   sqlite3_bind_text(st, 4, in->email, -1, SQLITE_TRANSIENT);   else sqlite3_bind_null(st, 4);
  if (in->phone)   sqlite3_bind_text(st, 5, in->phone, -1, SQLITE_TRANSIENT);   else sqlite3_bind_null(st, 5);
  if (in->address) sqlite3_bind_text(st, 6, in->address, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);
  if (in->note)    sqlite3_bind_text(st, 7, in->note, -1, SQLITE_TRANSIENT);    else sqlite3_bind_null(st, 7);
  sqlite3_bind_text(st, 8, created, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 9, seq);
  sqlite3_bind_int64(st, 10, lamport);

  if (sqlite3_step(st) != SQLITE_DONE) {
    e = MB_FAIL(MB_ERR_DB, "insert counterparty: %s", sqlite3_errmsg(mb_store_handle(s)));
    sqlite3_finalize(st);
    mb_store_rollback(s);
    return e;
  }
  sqlite3_finalize(st);
  MB_TRY(mb_store_commit(s));
  snprintf(id_out, 40, "%s", id);
  return MB_OK;
}

mb_err mb_counterparty_get(mb_store *s, const char *id, mb_counterparty *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT id,name,kind,email,phone,address,note,is_active FROM counterparty WHERE id=?;",
        -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);

  mb_err e;
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    memset(out, 0, sizeof *out);
    const char *col;
    snprintf(out->id, sizeof out->id, "%s", (const char *)sqlite3_column_text(st, 0));
    snprintf(out->name, sizeof out->name, "%s", (const char *)sqlite3_column_text(st, 1));
    out->kind = parse_kind((const char *)sqlite3_column_text(st, 2));
    col = (const char *)sqlite3_column_text(st, 3); snprintf(out->email, sizeof out->email, "%s", col ? col : "");
    col = (const char *)sqlite3_column_text(st, 4); snprintf(out->phone, sizeof out->phone, "%s", col ? col : "");
    col = (const char *)sqlite3_column_text(st, 5); snprintf(out->address, sizeof out->address, "%s", col ? col : "");
    col = (const char *)sqlite3_column_text(st, 6); snprintf(out->note, sizeof out->note, "%s", col ? col : "");
    out->is_active = sqlite3_column_int(st, 7);
    e = MB_OK;
  } else if (rc == SQLITE_DONE) {
    e = MB_FAIL(MB_ERR_NOT_FOUND, "counterparty '%s'", id);
  } else {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  }
  sqlite3_finalize(st);
  return e;
}

mb_err mb_counterparty_set_active(mb_store *s, const char *id, int active) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "UPDATE counterparty SET is_active=? WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_int(st, 1, active ? 1 : 0);
  sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  if (e == MB_OK && sqlite3_changes(mb_store_handle(s)) == 0)
    return MB_FAIL(MB_ERR_NOT_FOUND, "counterparty '%s'", id);
  return e;
}

mb_err mb_counterparty_count(mb_store *s, int *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT COUNT(*) FROM counterparty;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  mb_err e = (sqlite3_step(st) == SQLITE_ROW) ? (*out = sqlite3_column_int(st, 0), MB_OK)
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

static void fill_cp(sqlite3_stmt *st, mb_counterparty *out) {
  memset(out, 0, sizeof *out);
  const char *c;
  snprintf(out->id, sizeof out->id, "%s", (const char *)sqlite3_column_text(st, 0));
  snprintf(out->name, sizeof out->name, "%s", (const char *)sqlite3_column_text(st, 1));
  out->kind = parse_kind((const char *)sqlite3_column_text(st, 2));
  c = (const char *)sqlite3_column_text(st, 3); snprintf(out->email, sizeof out->email, "%s", c ? c : "");
  c = (const char *)sqlite3_column_text(st, 4); snprintf(out->phone, sizeof out->phone, "%s", c ? c : "");
  c = (const char *)sqlite3_column_text(st, 5); snprintf(out->address, sizeof out->address, "%s", c ? c : "");
  c = (const char *)sqlite3_column_text(st, 6); snprintf(out->note, sizeof out->note, "%s", c ? c : "");
  out->is_active = sqlite3_column_int(st, 7);
}

mb_err mb_counterparty_list(mb_store *s, int active_only, mb_counterparty **out, int *n) {
  const char *sql = active_only
    ? "SELECT id,name,kind,email,phone,address,note,is_active FROM counterparty WHERE is_active=1 ORDER BY name;"
    : "SELECT id,name,kind,email,phone,address,note,is_active FROM counterparty ORDER BY name;";
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), sql, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  int cap = 16, cnt = 0;
  mb_counterparty *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (cnt == cap) {
      cap *= 2;
      mb_counterparty *na = realloc(arr, (size_t)cap * sizeof *na);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    fill_cp(st, &arr[cnt++]);
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) { free(arr); return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  *out = arr;
  *n = cnt;
  return MB_OK;
}

#ifdef MB_TEST
#include "../support/mb_test.h"

TEST(counterparty, create_get) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  mb_counterparty_new in = {.name = "Acme Corp", .kind = MB_CP_CUSTOMER, .email = "ap@acme.test"};
  char id[40];
  ASSERT_OK(mb_counterparty_create(s, &in, id));
  mb_counterparty c;
  ASSERT_OK(mb_counterparty_get(s, id, &c));
  ASSERT_STR_EQ(c.name, "Acme Corp");
  ASSERT_STR_EQ(c.email, "ap@acme.test");
  ASSERT_EQ_INT(c.kind, MB_CP_CUSTOMER);
  ASSERT_EQ_INT(c.is_active, 1);
  mb_store_close(s);
}

TEST(counterparty, archive_and_missing) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  mb_counterparty_new in = {.name = "Old Vendor", .kind = MB_CP_VENDOR};
  char id[40];
  ASSERT_OK(mb_counterparty_create(s, &in, id));
  ASSERT_OK(mb_counterparty_set_active(s, id, 0));
  mb_counterparty c;
  ASSERT_OK(mb_counterparty_get(s, id, &c));
  ASSERT_EQ_INT(c.is_active, 0);
  ASSERT_ERR(mb_counterparty_get(s, "nope", &c), MB_ERR_NOT_FOUND);
  ASSERT_ERR(mb_counterparty_set_active(s, "nope", 1), MB_ERR_NOT_FOUND);
  mb_store_close(s);
}

TEST(counterparty, name_required) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  mb_counterparty_new in = {.name = "", .kind = MB_CP_CUSTOMER};
  char id[40];
  ASSERT_ERR(mb_counterparty_create(s, &in, id), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}
#endif
