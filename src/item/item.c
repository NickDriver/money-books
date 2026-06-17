#include "item.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../support/mb_id.h"

const char *mb_item_kind_str(mb_item_kind k) {
  return k == MB_ITEM_EXPENSE ? "EXPENSE" : "SERVICE";
}

mb_err mb_item_create(mb_store *s, const mb_item_new *in, char id_out[40]) {
  if (!in || !in->name || !in->name[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "item name required");

  char id[40], created[24];
  MB_TRY(mb_uuid(id, sizeof id));
  mb_now_iso(created, sizeof created);

  MB_TRY(mb_store_begin(s));
  int64_t seq, lamport;
  mb_err e = mb_store_next_stamp(s, &seq, &lamport);
  if (e != MB_OK) { mb_store_rollback(s); return e; }

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO item(id,kind,name,default_unit_price,default_account_id,unit_label,"
        "is_active,created_at,device_id,seq,lamport) VALUES(?,?,?,?,?,?,1,?,"
        "(SELECT v FROM book_meta WHERE k='device_id'),?,?);", -1, &st, NULL) != SQLITE_OK) {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
    mb_store_rollback(s);
    return e;
  }
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, mb_item_kind_str(in->kind), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, in->name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, in->default_unit_price);
  if (in->default_account_id) sqlite3_bind_text(st, 5, in->default_account_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
  if (in->unit_label) sqlite3_bind_text(st, 6, in->unit_label, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);
  sqlite3_bind_text(st, 7, created, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 8, seq);
  sqlite3_bind_int64(st, 9, lamport);

  int rc = sqlite3_step(st);
  if (rc != SQLITE_DONE) {
    /* FK failure on a bad default_account_id surfaces here */
    e = (sqlite3_extended_errcode(mb_store_handle(s)) == SQLITE_CONSTRAINT_FOREIGNKEY)
        ? MB_FAIL(MB_ERR_NOT_FOUND, "default_account_id does not exist")
        : MB_FAIL(MB_ERR_DB, "insert item: %s", sqlite3_errmsg(mb_store_handle(s)));
    sqlite3_finalize(st);
    mb_store_rollback(s);
    return e;
  }
  sqlite3_finalize(st);
  MB_TRY(mb_store_commit(s));
  snprintf(id_out, 40, "%s", id);
  return MB_OK;
}

mb_err mb_item_get(mb_store *s, const char *id, mb_item *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT id,kind,name,default_unit_price,default_account_id,unit_label,is_active "
        "FROM item WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);

  mb_err e;
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    memset(out, 0, sizeof *out);
    const char *col;
    snprintf(out->id, sizeof out->id, "%s", (const char *)sqlite3_column_text(st, 0));
    out->kind = strcmp((const char *)sqlite3_column_text(st, 1), "EXPENSE") == 0 ? MB_ITEM_EXPENSE : MB_ITEM_SERVICE;
    snprintf(out->name, sizeof out->name, "%s", (const char *)sqlite3_column_text(st, 2));
    out->default_unit_price = (mb_money)sqlite3_column_int64(st, 3);
    col = (const char *)sqlite3_column_text(st, 4); snprintf(out->default_account_id, sizeof out->default_account_id, "%s", col ? col : "");
    col = (const char *)sqlite3_column_text(st, 5); snprintf(out->unit_label, sizeof out->unit_label, "%s", col ? col : "");
    out->is_active = sqlite3_column_int(st, 6);
    e = MB_OK;
  } else if (rc == SQLITE_DONE) {
    e = MB_FAIL(MB_ERR_NOT_FOUND, "item '%s'", id);
  } else {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  }
  sqlite3_finalize(st);
  return e;
}

mb_err mb_item_set_active(mb_store *s, const char *id, int active) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), "UPDATE item SET is_active=? WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_int(st, 1, active ? 1 : 0);
  sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  if (e == MB_OK && sqlite3_changes(mb_store_handle(s)) == 0)
    return MB_FAIL(MB_ERR_NOT_FOUND, "item '%s'", id);
  return e;
}

mb_err mb_item_count(mb_store *s, int *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT COUNT(*) FROM item;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  mb_err e = (sqlite3_step(st) == SQLITE_ROW) ? (*out = sqlite3_column_int(st, 0), MB_OK)
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_item_list(mb_store *s, int active_only, mb_item **out, int *n) {
  const char *sql = active_only
    ? "SELECT id,kind,name,default_unit_price,default_account_id,unit_label,is_active FROM item WHERE is_active=1 ORDER BY name;"
    : "SELECT id,kind,name,default_unit_price,default_account_id,unit_label,is_active FROM item ORDER BY name;";
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), sql, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  int cap = 16, cnt = 0;
  mb_item *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (cnt == cap) {
      cap *= 2;
      mb_item *na = realloc(arr, (size_t)cap * sizeof *na);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    mb_item *r = &arr[cnt++];
    memset(r, 0, sizeof *r);
    const char *col;
    snprintf(r->id, sizeof r->id, "%s", (const char *)sqlite3_column_text(st, 0));
    r->kind = strcmp((const char *)sqlite3_column_text(st, 1), "EXPENSE") == 0 ? MB_ITEM_EXPENSE : MB_ITEM_SERVICE;
    snprintf(r->name, sizeof r->name, "%s", (const char *)sqlite3_column_text(st, 2));
    r->default_unit_price = (mb_money)sqlite3_column_int64(st, 3);
    col = (const char *)sqlite3_column_text(st, 4); snprintf(r->default_account_id, sizeof r->default_account_id, "%s", col ? col : "");
    col = (const char *)sqlite3_column_text(st, 5); snprintf(r->unit_label, sizeof r->unit_label, "%s", col ? col : "");
    r->is_active = sqlite3_column_int(st, 6);
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) { free(arr); return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  *out = arr;
  *n = cnt;
  return MB_OK;
}

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../account/account.h"

TEST(item, create_get_linked) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char income[40];
  mb_account_new acc = {.name = "Consulting Income", .type = MB_ACCT_INCOME, .role = MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &acc, income));

  mb_item_new in = {.kind = MB_ITEM_SERVICE, .name = "Hourly Consulting",
                    .default_unit_price = 15000, .default_account_id = income, .unit_label = "hour"};
  char id[40];
  ASSERT_OK(mb_item_create(s, &in, id));
  mb_item it;
  ASSERT_OK(mb_item_get(s, id, &it));
  ASSERT_STR_EQ(it.name, "Hourly Consulting");
  ASSERT_MONEY_EQ(it.default_unit_price, 15000);
  ASSERT_STR_EQ(it.default_account_id, income);
  ASSERT_STR_EQ(it.unit_label, "hour");
  ASSERT_EQ_INT(it.kind, MB_ITEM_SERVICE);
  mb_store_close(s);
}

TEST(item, bad_account_rejected) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  mb_item_new in = {.kind = MB_ITEM_SERVICE, .name = "X", .default_account_id = "ghost"};
  char id[40];
  ASSERT_ERR(mb_item_create(s, &in, id), MB_ERR_NOT_FOUND);
  mb_store_close(s);
}

TEST(item, name_required) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  mb_item_new in = {.kind = MB_ITEM_EXPENSE, .name = ""};
  char id[40];
  ASSERT_ERR(mb_item_create(s, &in, id), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}

TEST(item, list_and_active_filter) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char income[40];
  mb_account_new acc = {.name = "Income", .type = MB_ACCT_INCOME, .role = MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &acc, income));
  char a[40], b[40];
  mb_item_new i1 = {.kind = MB_ITEM_SERVICE, .name = "Design", .default_unit_price = 20000, .default_account_id = income};
  mb_item_new i2 = {.kind = MB_ITEM_SERVICE, .name = "Dev",    .default_unit_price = 30000, .default_account_id = income};
  ASSERT_OK(mb_item_create(s, &i1, a));
  ASSERT_OK(mb_item_create(s, &i2, b));
  ASSERT_OK(mb_item_set_active(s, b, 0));   /* archive Dev */

  mb_item *rows = NULL; int n = 0;
  ASSERT_OK(mb_item_list(s, 0, &rows, &n));
  ASSERT_EQ_INT(n, 2);                       /* both when not active_only */
  free(rows);
  ASSERT_OK(mb_item_list(s, 1, &rows, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_STR_EQ(rows[0].name, "Design");
  ASSERT_MONEY_EQ(rows[0].default_unit_price, 20000);
  free(rows);
  mb_store_close(s);
}
#endif
