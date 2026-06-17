#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../support/mb_id.h"

struct mb_store {
  sqlite3 *db;
};

/* ---- schema migrations (index i builds version i+1) ---- */
static const char *const MIGRATIONS[] = {
  /* v1 — Phase 1 core */
  "CREATE TABLE book_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);"
  "CREATE TABLE account ("
  "  id TEXT PRIMARY KEY,"
  "  code TEXT,"
  "  name TEXT NOT NULL,"
  "  type TEXT NOT NULL CHECK(type IN ('ASSET','LIABILITY','EQUITY','INCOME','EXPENSE')),"
  "  role TEXT NOT NULL CHECK(role IN ('SYSTEM','ACCOUNT','CATEGORY')),"
  "  parent_id TEXT REFERENCES account(id),"
  "  currency TEXT NOT NULL,"
  "  is_active INTEGER NOT NULL DEFAULT 1,"
  "  created_at TEXT NOT NULL,"
  "  device_id TEXT NOT NULL, seq INTEGER NOT NULL, lamport INTEGER NOT NULL);"
  "CREATE INDEX idx_account_type ON account(type);"
  "CREATE TABLE journal_entry ("
  "  id TEXT PRIMARY KEY,"
  "  date TEXT NOT NULL,"
  "  memo TEXT,"
  "  status TEXT NOT NULL DEFAULT 'POSTED' CHECK(status IN ('POSTED','REVERSED','REVERSAL')),"
  "  reverses_id TEXT REFERENCES journal_entry(id),"
  "  source TEXT NOT NULL DEFAULT 'USER' CHECK(source IN ('USER','AI','IMPORT')),"
  "  created_at TEXT NOT NULL,"
  "  device_id TEXT NOT NULL, seq INTEGER NOT NULL, lamport INTEGER NOT NULL,"
  "  content_hash TEXT NOT NULL, prev_hash TEXT);"
  "CREATE INDEX idx_entry_date ON journal_entry(date);"
  "CREATE TABLE posting ("
  "  id TEXT PRIMARY KEY,"
  "  entry_id TEXT NOT NULL REFERENCES journal_entry(id),"
  "  account_id TEXT NOT NULL REFERENCES account(id),"
  "  amount INTEGER NOT NULL,"   /* signed cents: + debit, - credit */
  "  memo TEXT);"
  "CREATE INDEX idx_posting_entry ON posting(entry_id);"
  "CREATE INDEX idx_posting_account ON posting(account_id);",

  /* v2 — Phase 2: counterparties, items, invoices/bills, payments */
  "CREATE TABLE counterparty ("
  "  id TEXT PRIMARY KEY,"
  "  name TEXT NOT NULL,"
  "  kind TEXT NOT NULL CHECK(kind IN ('CUSTOMER','VENDOR','BOTH')),"
  "  email TEXT, phone TEXT, address TEXT, note TEXT,"
  "  is_active INTEGER NOT NULL DEFAULT 1,"
  "  created_at TEXT NOT NULL,"
  "  device_id TEXT NOT NULL, seq INTEGER NOT NULL, lamport INTEGER NOT NULL);"
  "CREATE TABLE item ("
  "  id TEXT PRIMARY KEY,"
  "  kind TEXT NOT NULL CHECK(kind IN ('SERVICE','EXPENSE')),"
  "  name TEXT NOT NULL,"
  "  default_unit_price INTEGER NOT NULL DEFAULT 0,"   /* cents */
  "  default_account_id TEXT REFERENCES account(id),"
  "  unit_label TEXT,"
  "  is_active INTEGER NOT NULL DEFAULT 1,"
  "  created_at TEXT NOT NULL,"
  "  device_id TEXT NOT NULL, seq INTEGER NOT NULL, lamport INTEGER NOT NULL);"
  "CREATE TABLE invoice ("
  "  id TEXT PRIMARY KEY,"
  "  number TEXT,"
  "  counterparty_id TEXT NOT NULL REFERENCES counterparty(id),"
  "  issue_date TEXT, due_date TEXT,"
  "  status TEXT NOT NULL DEFAULT 'DRAFT' CHECK(status IN ('DRAFT','OPEN','PARTIAL','PAID','VOID')),"
  "  memo TEXT, currency TEXT NOT NULL,"
  "  entry_id TEXT REFERENCES journal_entry(id),"
  "  created_at TEXT NOT NULL,"
  "  device_id TEXT NOT NULL, seq INTEGER NOT NULL, lamport INTEGER NOT NULL);"
  "CREATE TABLE invoice_line ("
  "  id TEXT PRIMARY KEY,"
  "  invoice_id TEXT NOT NULL REFERENCES invoice(id),"
  "  item_id TEXT REFERENCES item(id),"
  "  description TEXT NOT NULL,"
  "  qty_centi INTEGER NOT NULL DEFAULT 100,"   /* quantity x100 (1.5 -> 150) */
  "  unit_price INTEGER NOT NULL,"              /* cents */
  "  line_total INTEGER NOT NULL,"              /* cents */
  "  account_id TEXT NOT NULL REFERENCES account(id),"
  "  is_tax INTEGER NOT NULL DEFAULT 0);"
  "CREATE INDEX idx_invline_invoice ON invoice_line(invoice_id);"
  "CREATE TABLE bill ("
  "  id TEXT PRIMARY KEY,"
  "  number TEXT,"
  "  counterparty_id TEXT NOT NULL REFERENCES counterparty(id),"
  "  issue_date TEXT, due_date TEXT,"
  "  status TEXT NOT NULL DEFAULT 'DRAFT' CHECK(status IN ('DRAFT','OPEN','PARTIAL','PAID','VOID')),"
  "  memo TEXT, currency TEXT NOT NULL,"
  "  entry_id TEXT REFERENCES journal_entry(id),"
  "  created_at TEXT NOT NULL,"
  "  device_id TEXT NOT NULL, seq INTEGER NOT NULL, lamport INTEGER NOT NULL);"
  "CREATE TABLE bill_line ("
  "  id TEXT PRIMARY KEY,"
  "  bill_id TEXT NOT NULL REFERENCES bill(id),"
  "  item_id TEXT REFERENCES item(id),"
  "  description TEXT NOT NULL,"
  "  qty_centi INTEGER NOT NULL DEFAULT 100,"
  "  unit_price INTEGER NOT NULL,"
  "  line_total INTEGER NOT NULL,"
  "  account_id TEXT NOT NULL REFERENCES account(id),"
  "  is_tax INTEGER NOT NULL DEFAULT 0);"
  "CREATE INDEX idx_billline_bill ON bill_line(bill_id);"
  "CREATE TABLE payment ("
  "  id TEXT PRIMARY KEY,"
  "  date TEXT NOT NULL,"
  "  amount INTEGER NOT NULL,"           /* cents, positive */
  "  account_id TEXT NOT NULL REFERENCES account(id),"
  "  target_kind TEXT NOT NULL CHECK(target_kind IN ('INVOICE','BILL')),"
  "  target_id TEXT NOT NULL,"
  "  entry_id TEXT REFERENCES journal_entry(id),"
  "  created_at TEXT NOT NULL,"
  "  device_id TEXT NOT NULL, seq INTEGER NOT NULL, lamport INTEGER NOT NULL);"
  "CREATE INDEX idx_payment_target ON payment(target_kind, target_id);",

  /* v3 — per-tool AI permission policy (D18) */
  "CREATE TABLE tool_permission ("
  "  tool TEXT PRIMARY KEY,"
  "  policy TEXT NOT NULL CHECK(policy IN ('PERMIT','ASK','BLOCK')));",

  /* v4 — local app settings (non-secret; API keys live in mb_secret_store, never here) */
  "CREATE TABLE app_setting (k TEXT PRIMARY KEY, v TEXT NOT NULL);",

  /* v5 — customer/vendor credit (D26): per-counterparty AR/AP balance-forward + manual
   * application of available credit to open documents.
   *  - posting.counterparty_id tags AR/AP control postings so a counterparty's running
   *    balance (Σ of its AR/AP postings) is computable; net-negative AR = customer in credit.
   *  - payment.counterparty_id denormalizes the doc's counterparty for credit-pool queries.
   *  - allocation is the open-item settlement ledger: every dollar applied to a document
   *    (from a cash payment OR from available credit) is one allocation row. A document's
   *    settled amount = Σ allocations to it (drives PARTIAL/PAID). A counterparty's available
   *    credit = Σ its payments − Σ its allocations. No journal entry for a credit application
   *    (it only reallocates AR that already exists). */
  "ALTER TABLE posting ADD COLUMN counterparty_id TEXT REFERENCES counterparty(id);"
  "ALTER TABLE payment ADD COLUMN counterparty_id TEXT REFERENCES counterparty(id);"
  "CREATE TABLE allocation ("
  "  id TEXT PRIMARY KEY,"
  "  date TEXT NOT NULL,"
  "  counterparty_id TEXT NOT NULL REFERENCES counterparty(id),"
  "  source_kind TEXT NOT NULL CHECK(source_kind IN ('PAYMENT','CREDIT')),"
  "  payment_id TEXT REFERENCES payment(id),"   /* set when source_kind='PAYMENT' */
  "  target_kind TEXT NOT NULL CHECK(target_kind IN ('INVOICE','BILL')),"
  "  target_id TEXT NOT NULL,"
  "  amount INTEGER NOT NULL,"                  /* positive cents applied to the document */
  "  created_at TEXT NOT NULL,"
  "  device_id TEXT NOT NULL, seq INTEGER NOT NULL, lamport INTEGER NOT NULL);"
  "CREATE INDEX idx_alloc_target ON allocation(target_kind, target_id);"
  "CREATE INDEX idx_alloc_cp ON allocation(counterparty_id, target_kind);"
  "CREATE INDEX idx_posting_cp ON posting(counterparty_id);",
};
static const int MIGRATION_COUNT = (int)(sizeof MIGRATIONS / sizeof MIGRATIONS[0]);

static mb_err exec_raw(sqlite3 *db, const char *sql) {
  char *err = NULL;
  if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
    mb_err e = MB_FAIL(MB_ERR_DB, "%s", err ? err : sqlite3_errmsg(db));
    sqlite3_free(err);
    return e;
  }
  return MB_OK;
}

void mb_now_iso(char *buf, size_t buflen) {
  time_t t = time(NULL);
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

sqlite3 *mb_store_handle(mb_store *s) { return s->db; }

mb_err mb_store_exec(mb_store *s, const char *sql) { return exec_raw(s->db, sql); }

mb_err mb_store_begin(mb_store *s)    { return exec_raw(s->db, "BEGIN IMMEDIATE;"); }
mb_err mb_store_commit(mb_store *s)   { return exec_raw(s->db, "COMMIT;"); }
void   mb_store_rollback(mb_store *s) { char *e = NULL; sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, &e); sqlite3_free(e); }

/* ---- metadata ---- */
mb_err mb_store_meta_get(mb_store *s, const char *key, char *buf, size_t buflen) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(s->db, "SELECT v FROM book_meta WHERE k=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(s->db));
  sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
  mb_err e;
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(st, 0);
    snprintf(buf, buflen, "%s", v ? v : "");
    e = MB_OK;
  } else if (rc == SQLITE_DONE) {
    e = MB_FAIL(MB_ERR_NOT_FOUND, "meta key '%s'", key);
  } else {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(s->db));
  }
  sqlite3_finalize(st);
  return e;
}

mb_err mb_store_meta_set(mb_store *s, const char *key, const char *val) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(s->db,
        "INSERT INTO book_meta(k,v) VALUES(?,?) "
        "ON CONFLICT(k) DO UPDATE SET v=excluded.v;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(s->db));
  sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, val, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(s->db));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_store_meta_get_int(mb_store *s, const char *key, int64_t *out) {
  char buf[32];
  MB_TRY(mb_store_meta_get(s, key, buf, sizeof buf));
  *out = (int64_t)strtoll(buf, NULL, 10);
  return MB_OK;
}

mb_err mb_store_meta_set_int(mb_store *s, const char *key, int64_t val) {
  char buf[32];
  snprintf(buf, sizeof buf, "%lld", (long long)val);
  return mb_store_meta_set(s, key, buf);
}

mb_err mb_store_next_stamp(mb_store *s, int64_t *seq, int64_t *lamport) {
  int64_t ns, lam;
  MB_TRY(mb_store_meta_get_int(s, "next_seq", &ns));
  MB_TRY(mb_store_meta_get_int(s, "lamport", &lam));
  *seq = ns;
  *lamport = lam + 1;
  MB_TRY(mb_store_meta_set_int(s, "next_seq", ns + 1));
  MB_TRY(mb_store_meta_set_int(s, "lamport", *lamport));
  return MB_OK;
}

mb_err mb_store_device_id(mb_store *s, char buf[40]) {
  return mb_store_meta_get(s, "device_id", buf, 40);
}
mb_err mb_store_currency(mb_store *s, char buf[8]) {
  return mb_store_meta_get(s, "currency", buf, 8);
}

/* ---- open / migrate / init ---- */
static mb_err migrate(sqlite3 *db) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(db));
  int version = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
  sqlite3_finalize(st);

  for (int v = version; v < MIGRATION_COUNT; v++) {
    MB_TRY(exec_raw(db, "BEGIN IMMEDIATE;"));
    mb_err e = exec_raw(db, MIGRATIONS[v]);
    if (e == MB_OK) {
      char pragma[48];
      snprintf(pragma, sizeof pragma, "PRAGMA user_version=%d;", v + 1);
      e = exec_raw(db, pragma);
    }
    if (e != MB_OK) { exec_raw(db, "ROLLBACK;"); return e; }
    MB_TRY(exec_raw(db, "COMMIT;"));
  }
  return MB_OK;
}

static mb_err init_meta(mb_store *s) {
  char buf[40];
  if (mb_store_meta_get(s, "device_id", buf, sizeof buf) == MB_ERR_NOT_FOUND) {
    char uuid[40];
    MB_TRY(mb_uuid(uuid, sizeof uuid));
    MB_TRY(mb_store_begin(s));
    mb_err e = mb_store_meta_set(s, "device_id", uuid);
    if (e == MB_OK) e = mb_store_meta_set_int(s, "next_seq", 1);
    if (e == MB_OK) e = mb_store_meta_set_int(s, "lamport", 0);
    if (e == MB_OK) e = mb_store_meta_set(s, "currency", "USD");
    if (e != MB_OK) { mb_store_rollback(s); return e; }
    MB_TRY(mb_store_commit(s));
  }
  return MB_OK;
}

static mb_err open_common(const char *path, mb_store **out) {
  mb_store *s = calloc(1, sizeof *s);
  if (!s) return MB_FAIL(MB_ERR_INTERNAL, "oom");
  if (sqlite3_open(path, &s->db) != SQLITE_OK) {
    mb_err e = MB_FAIL(MB_ERR_DB, "open '%s': %s", path, sqlite3_errmsg(s->db));
    sqlite3_close(s->db);
    free(s);
    return e;
  }
  sqlite3_busy_timeout(s->db, 5000);
  mb_err e = exec_raw(s->db, "PRAGMA journal_mode=WAL;");
  if (e == MB_OK) e = exec_raw(s->db, "PRAGMA foreign_keys=ON;");
  if (e == MB_OK) e = migrate(s->db);
  if (e == MB_OK) e = init_meta(s);
  if (e != MB_OK) { mb_store_close(s); return e; }
  *out = s;
  return MB_OK;
}

mb_err mb_store_open(const char *path, mb_store **out) { return open_common(path, out); }
mb_err mb_store_open_memory(mb_store **out)            { return open_common(":memory:", out); }

void mb_store_close(mb_store *s) {
  if (!s) return;
  if (s->db) sqlite3_close(s->db);
  free(s);
}

#ifdef MB_TEST
#include "../support/mb_test.h"

TEST(store, open_and_meta) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  char dev[40], cur[8];
  ASSERT_OK(mb_store_device_id(s, dev));
  ASSERT_OK(mb_store_currency(s, cur));
  ASSERT_EQ_INT((long)strlen(dev), 36);
  ASSERT_STR_EQ(cur, "USD");
  mb_store_close(s);
}

TEST(store, stamps_increment) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  int64_t s1, l1, s2, l2;
  ASSERT_OK(mb_store_begin(s));
  ASSERT_OK(mb_store_next_stamp(s, &s1, &l1));
  ASSERT_OK(mb_store_next_stamp(s, &s2, &l2));
  ASSERT_OK(mb_store_commit(s));
  EXPECT(s2 == s1 + 1);
  EXPECT(l2 == l1 + 1);
  mb_store_close(s);
}

TEST(store, meta_missing_is_not_found) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  char buf[16];
  ASSERT_ERR(mb_store_meta_get(s, "nope", buf, sizeof buf), MB_ERR_NOT_FOUND);
  mb_store_close(s);
}
#endif
