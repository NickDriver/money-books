#include "account.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../support/mb_id.h"

const char *mb_account_type_str(mb_account_type t) {
  switch (t) {
    case MB_ACCT_ASSET:     return "ASSET";
    case MB_ACCT_LIABILITY: return "LIABILITY";
    case MB_ACCT_EQUITY:    return "EQUITY";
    case MB_ACCT_INCOME:    return "INCOME";
    case MB_ACCT_EXPENSE:   return "EXPENSE";
  }
  return "ASSET";
}

const char *mb_account_role_str(mb_account_role r) {
  switch (r) {
    case MB_ROLE_SYSTEM:   return "SYSTEM";
    case MB_ROLE_ACCOUNT:  return "ACCOUNT";
    case MB_ROLE_CATEGORY: return "CATEGORY";
  }
  return "ACCOUNT";
}

static mb_err parse_type(const char *s, mb_account_type *out) {
  if (!strcmp(s, "ASSET"))     { *out = MB_ACCT_ASSET;     return MB_OK; }
  if (!strcmp(s, "LIABILITY")) { *out = MB_ACCT_LIABILITY; return MB_OK; }
  if (!strcmp(s, "EQUITY"))    { *out = MB_ACCT_EQUITY;    return MB_OK; }
  if (!strcmp(s, "INCOME"))    { *out = MB_ACCT_INCOME;    return MB_OK; }
  if (!strcmp(s, "EXPENSE"))   { *out = MB_ACCT_EXPENSE;   return MB_OK; }
  return MB_FAIL(MB_ERR_PARSE, "bad account type '%s'", s);
}

static mb_err parse_role(const char *s, mb_account_role *out) {
  if (!strcmp(s, "SYSTEM"))   { *out = MB_ROLE_SYSTEM;   return MB_OK; }
  if (!strcmp(s, "ACCOUNT"))  { *out = MB_ROLE_ACCOUNT;  return MB_OK; }
  if (!strcmp(s, "CATEGORY")) { *out = MB_ROLE_CATEGORY; return MB_OK; }
  return MB_FAIL(MB_ERR_PARSE, "bad account role '%s'", s);
}

mb_err mb_account_create(mb_store *s, const mb_account_new *in, char id_out[40]) {
  if (!in || !in->name || !in->name[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "account name required");

  char id[40], created[24], currency[8];
  MB_TRY(mb_uuid(id, sizeof id));
  mb_now_iso(created, sizeof created);
  if (in->currency && in->currency[0]) snprintf(currency, sizeof currency, "%s", in->currency);
  else MB_TRY(mb_store_currency(s, currency));

  MB_TRY(mb_store_begin(s));
  int64_t seq, lamport;
  mb_err e = mb_store_next_stamp(s, &seq, &lamport);
  if (e != MB_OK) { mb_store_rollback(s); return e; }

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO account(id,code,name,type,role,parent_id,currency,is_active,created_at,"
        "device_id,seq,lamport) VALUES(?,?,?,?,?,?,?,1,?,"
        "(SELECT v FROM book_meta WHERE k='device_id'),?,?);", -1, &st, NULL) != SQLITE_OK) {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
    mb_store_rollback(s);
    return e;
  }
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  if (in->code) sqlite3_bind_text(st, 2, in->code, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 2);
  sqlite3_bind_text(st, 3, in->name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, mb_account_type_str(in->type), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, mb_account_role_str(in->role), -1, SQLITE_TRANSIENT);
  if (in->parent_id) sqlite3_bind_text(st, 6, in->parent_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);
  sqlite3_bind_text(st, 7, currency, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, created, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 9, seq);
  sqlite3_bind_int64(st, 10, lamport);

  if (sqlite3_step(st) != SQLITE_DONE) {
    e = MB_FAIL(MB_ERR_DB, "insert account: %s", sqlite3_errmsg(mb_store_handle(s)));
    sqlite3_finalize(st);
    mb_store_rollback(s);
    return e;
  }
  sqlite3_finalize(st);
  MB_TRY(mb_store_commit(s));

  snprintf(id_out, 40, "%s", id);
  return MB_OK;
}

/* SELECT column order shared by get/find/list */
#define ACCOUNT_COLS "id,code,name,type,role,parent_id,currency,is_active"

static void fill_account(sqlite3_stmt *st, mb_account *out) {
  memset(out, 0, sizeof *out);
  const char *col;
  snprintf(out->id, sizeof out->id, "%s", (const char *)sqlite3_column_text(st, 0));
  col = (const char *)sqlite3_column_text(st, 1);
  snprintf(out->code, sizeof out->code, "%s", col ? col : "");
  snprintf(out->name, sizeof out->name, "%s", (const char *)sqlite3_column_text(st, 2));
  (void)parse_type((const char *)sqlite3_column_text(st, 3), &out->type);  /* DB CHECK-constrained */
  (void)parse_role((const char *)sqlite3_column_text(st, 4), &out->role);
  col = (const char *)sqlite3_column_text(st, 5);
  snprintf(out->parent_id, sizeof out->parent_id, "%s", col ? col : "");
  snprintf(out->currency, sizeof out->currency, "%s", (const char *)sqlite3_column_text(st, 6));
  out->is_active = sqlite3_column_int(st, 7);
}

static mb_err get_one(mb_store *s, const char *col, const char *val, mb_account *out) {
  char sql[96];
  snprintf(sql, sizeof sql, "SELECT " ACCOUNT_COLS " FROM account WHERE %s=?;", col);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), sql, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, val, -1, SQLITE_TRANSIENT);
  mb_err e;
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW)       { fill_account(st, out); e = MB_OK; }
  else if (rc == SQLITE_DONE) { e = MB_FAIL(MB_ERR_NOT_FOUND, "account %s='%s'", col, val); }
  else                        { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  sqlite3_finalize(st);
  return e;
}

mb_err mb_account_get(mb_store *s, const char *id, mb_account *out) {
  return get_one(s, "id", id, out);
}

mb_err mb_account_find_by_code(mb_store *s, const char *code, mb_account *out) {
  return get_one(s, "code", code, out);
}

mb_err mb_account_list(mb_store *s, const mb_account_filter *f, mb_account **out, int *count) {
  char sql[256];
  int p = snprintf(sql, sizeof sql, "SELECT " ACCOUNT_COLS " FROM account WHERE 1=1");
  if (f && f->has_type)    p += snprintf(sql + p, sizeof sql - (size_t)p, " AND type=?");
  if (f && f->has_role)    p += snprintf(sql + p, sizeof sql - (size_t)p, " AND role=?");
  if (f && f->active_only) p += snprintf(sql + p, sizeof sql - (size_t)p, " AND is_active=1");
  snprintf(sql + p, sizeof sql - (size_t)p, " ORDER BY code, name;");

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), sql, -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  int bind = 1;
  if (f && f->has_type) sqlite3_bind_text(st, bind++, mb_account_type_str(f->type), -1, SQLITE_TRANSIENT);
  if (f && f->has_role) sqlite3_bind_text(st, bind++, mb_account_role_str(f->role), -1, SQLITE_TRANSIENT);

  int cap = 16, n = 0;
  mb_account *arr = malloc((size_t)cap * sizeof *arr);
  if (!arr) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (n == cap) {
      cap *= 2;
      mb_account *na = realloc(arr, (size_t)cap * sizeof *arr);
      if (!na) { free(arr); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      arr = na;
    }
    fill_account(st, &arr[n++]);
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) { free(arr); return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  *out = arr;
  *count = n;
  return MB_OK;
}

mb_err mb_account_set_active(mb_store *s, const char *id, int active) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "UPDATE account SET is_active=? WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_int(st, 1, active ? 1 : 0);
  sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  if (e == MB_OK && sqlite3_changes(mb_store_handle(s)) == 0)
    return MB_FAIL(MB_ERR_NOT_FOUND, "account '%s'", id);
  return e;
}

mb_err mb_account_update(mb_store *s, const char *id, const char *code, const char *name) {
  if (!name || !name[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "name required");
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "UPDATE account SET code=?, name=? WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, code ? code : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  if (e == MB_OK && sqlite3_changes(mb_store_handle(s)) == 0)
    return MB_FAIL(MB_ERR_NOT_FOUND, "account '%s'", id);
  return e;
}

mb_err mb_account_count(mb_store *s, int *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT COUNT(*) FROM account;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  mb_err e = (sqlite3_step(st) == SQLITE_ROW) ? (*out = sqlite3_column_int(st, 0), MB_OK)
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

#ifdef MB_TEST
#include "../support/mb_test.h"

static mb_err mk(mb_store *s, const char *name, mb_account_type t, mb_account_role r, char id[40]) {
  mb_account_new in = {.name = name, .type = t, .role = r};
  return mb_account_create(s, &in, id);
}

TEST(account, create_get_roundtrip) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  mb_account_new in = {.code = "1000", .name = "Business Checking",
                       .type = MB_ACCT_ASSET, .role = MB_ROLE_ACCOUNT};
  char id[40];
  ASSERT_OK(mb_account_create(s, &in, id));
  mb_account a;
  ASSERT_OK(mb_account_get(s, id, &a));
  ASSERT_STR_EQ(a.name, "Business Checking");
  ASSERT_STR_EQ(a.code, "1000");
  ASSERT_EQ_INT(a.type, MB_ACCT_ASSET);
  ASSERT_EQ_INT(a.role, MB_ROLE_ACCOUNT);
  ASSERT_STR_EQ(a.currency, "USD");
  ASSERT_EQ_INT(a.is_active, 1);
  mb_store_close(s);
}

TEST(account, archive) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  char id[40];
  ASSERT_OK(mk(s, "Old Account", MB_ACCT_EXPENSE, MB_ROLE_CATEGORY, id));
  ASSERT_OK(mb_account_set_active(s, id, 0));
  mb_account a;
  ASSERT_OK(mb_account_get(s, id, &a));
  ASSERT_EQ_INT(a.is_active, 0);
  mb_store_close(s);
}

TEST(account, get_missing_is_not_found) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  mb_account a;
  ASSERT_ERR(mb_account_get(s, "no-such-id", &a), MB_ERR_NOT_FOUND);
  ASSERT_ERR(mb_account_set_active(s, "no-such-id", 0), MB_ERR_NOT_FOUND);
  mb_store_close(s);
}

TEST(account, name_required) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  mb_account_new in = {.name = "", .type = MB_ACCT_ASSET, .role = MB_ROLE_ACCOUNT};
  char id[40];
  ASSERT_ERR(mb_account_create(s, &in, id), MB_ERR_INVALID_ARG);
  mb_store_close(s);
}

TEST(account, distinct_seqs) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  char a[40], b[40];
  ASSERT_OK(mk(s, "A", MB_ACCT_ASSET, MB_ROLE_ACCOUNT, a));
  ASSERT_OK(mk(s, "B", MB_ACCT_ASSET, MB_ROLE_ACCOUNT, b));
  ASSERT(strcmp(a, b) != 0);
  int n = 0;
  ASSERT_OK(mb_account_count(s, &n));
  ASSERT_EQ_INT(n, 2);
  mb_store_close(s);
}

TEST(account, find_by_code) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  mb_account_new in = {.code = "1200", .name = "Accounts Receivable",
                       .type = MB_ACCT_ASSET, .role = MB_ROLE_SYSTEM};
  char id[40];
  ASSERT_OK(mb_account_create(s, &in, id));
  mb_account a;
  ASSERT_OK(mb_account_find_by_code(s, "1200", &a));
  ASSERT_STR_EQ(a.id, id);
  ASSERT_EQ_INT(a.role, MB_ROLE_SYSTEM);
  ASSERT_ERR(mb_account_find_by_code(s, "9999", &a), MB_ERR_NOT_FOUND);
  mb_store_close(s);
}

TEST(account, list_filters) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  char id[40];
  ASSERT_OK(mk(s, "Cash",     MB_ACCT_ASSET,   MB_ROLE_ACCOUNT,  id));
  ASSERT_OK(mk(s, "Income A", MB_ACCT_INCOME,  MB_ROLE_CATEGORY, id));
  ASSERT_OK(mk(s, "Income B", MB_ACCT_INCOME,  MB_ROLE_CATEGORY, id));
  ASSERT_OK(mb_account_set_active(s, id, 0));  /* archive Income B */

  mb_account *list = NULL; int n = 0;
  /* all income */
  mb_account_filter f1 = {.has_type = 1, .type = MB_ACCT_INCOME};
  ASSERT_OK(mb_account_list(s, &f1, &list, &n));
  ASSERT_EQ_INT(n, 2);
  free(list);
  /* active income only */
  mb_account_filter f2 = {.has_type = 1, .type = MB_ACCT_INCOME, .active_only = 1};
  ASSERT_OK(mb_account_list(s, &f2, &list, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_STR_EQ(list[0].name, "Income A");
  free(list);
  /* everything */
  ASSERT_OK(mb_account_list(s, NULL, &list, &n));
  ASSERT_EQ_INT(n, 3);
  free(list);
  mb_store_close(s);
}

TEST(account, update_renames_and_recodes) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char id[40];
  ASSERT_OK(mk(s, "Conslting Incom", MB_ACCT_INCOME, MB_ROLE_CATEGORY, id));  /* typo */
  ASSERT_OK(mb_account_update(s, id, "4000", "Consulting Income"));
  mb_account a; ASSERT_OK(mb_account_get(s, id, &a));
  ASSERT_STR_EQ(a.name, "Consulting Income");
  ASSERT_STR_EQ(a.code, "4000");
  ASSERT_EQ_INT(a.type, MB_ACCT_INCOME);   /* type unchanged */
  ASSERT_ERR(mb_account_update(s, id, "4000", NULL), MB_ERR_INVALID_ARG);   /* name required */
  ASSERT_ERR(mb_account_update(s, "nope", "1", "x"), MB_ERR_NOT_FOUND);
  mb_store_close(s);
}
#endif
