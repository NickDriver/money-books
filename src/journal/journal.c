#include "journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../support/mb_id.h"
#include "../support/sha256.h"

const char *mb_source_str(mb_source src) {
  switch (src) {
    case MB_SRC_USER:   return "USER";
    case MB_SRC_AI:     return "AI";
    case MB_SRC_IMPORT: return "IMPORT";
  }
  return "USER";
}

static int cmp_posting(const void *A, const void *B) {
  const mb_posting_in *a = A, *b = B;
  int c = strcmp(a->account_id, b->account_id);
  if (c) return c;
  if (a->amount < b->amount) return -1;
  if (a->amount > b->amount) return 1;
  const char *am = a->memo ? a->memo : "", *bm = b->memo ? b->memo : "";
  return strcmp(am, bm);
}

static mb_err check_account(mb_store *s, const char *id) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT is_active FROM account WHERE id=?;",
                         -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  mb_err e;
  if (rc == SQLITE_ROW) {
    e = sqlite3_column_int(st, 0) ? MB_OK : MB_FAIL(MB_ERR_INVALID_ARG, "account %s is archived", id);
  } else if (rc == SQLITE_DONE) {
    e = MB_FAIL(MB_ERR_NOT_FOUND, "account %s", id);
  } else {
    e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  }
  sqlite3_finalize(st);
  return e;
}

/* shared insert path for normal posts and reversals — assumes caller holds a txn */
static mb_err post_core(mb_store *s, const char *date, const char *memo, mb_source src,
                        const char *status, const char *reverses_id,
                        const mb_posting_in *in, int n, char id_out[40]) {
  if (!date || !date[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "date required");
  if (n < 2) return MB_FAIL(MB_ERR_INVALID_ARG, "an entry needs >= 2 postings");

  /* balance check with overflow-safe sum */
  mb_money sum = 0;
  for (int i = 0; i < n; i++) MB_TRY(mb_money_add(sum, in[i].amount, &sum));
  if (sum != 0) return MB_FAIL(MB_ERR_UNBALANCED, "postings sum to %lld, not 0", (long long)sum);

  for (int i = 0; i < n; i++) MB_TRY(check_account(s, in[i].account_id));

  /* deterministic ordering for the hash */
  mb_posting_in *sorted = malloc((size_t)n * sizeof *sorted);
  if (!sorted) return MB_FAIL(MB_ERR_INTERNAL, "oom");
  memcpy(sorted, in, (size_t)n * sizeof *sorted);
  qsort(sorted, (size_t)n, sizeof *sorted, cmp_posting);

  char entry_id[40], created[24], device[40];
  mb_err e = mb_uuid(entry_id, sizeof entry_id);
  if (e != MB_OK) { free(sorted); return e; }
  mb_now_iso(created, sizeof created);

  int64_t seq, lamport;
  char prev_hash[65] = "";
  int has_prev = 1;
  if ((e = mb_store_next_stamp(s, &seq, &lamport)) != MB_OK) goto fail;
  if ((e = mb_store_device_id(s, device)) != MB_OK) goto fail;
  {
    mb_err pe = mb_store_meta_get(s, "last_hash", prev_hash, sizeof prev_hash);
    if (pe == MB_ERR_NOT_FOUND) { has_prev = 0; prev_hash[0] = '\0'; }
    else if (pe != MB_OK) { e = pe; goto fail; }
  }

  /* content hash over canonical serialization */
  char hash[65];
  {
    mb_sha256 h;
    mb_sha256_init(&h);
    char line[256];
    int m = snprintf(line, sizeof line, "%s\n%s\n%s\n%s\n%lld\n%lld\n%s\n",
                     date, memo ? memo : "", mb_source_str(src), device,
                     (long long)seq, (long long)lamport, prev_hash);
    mb_sha256_update(&h, line, (size_t)m);
    for (int i = 0; i < n; i++) {
      m = snprintf(line, sizeof line, "%s\t%lld\t%s\n", sorted[i].account_id,
                   (long long)sorted[i].amount, sorted[i].memo ? sorted[i].memo : "");
      mb_sha256_update(&h, line, (size_t)m);
    }
    mb_sha256_final_hex(&h, hash);
  }

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO journal_entry(id,date,memo,status,reverses_id,source,created_at,"
        "device_id,seq,lamport,content_hash,prev_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?,?);",
        -1, &st, NULL) != SQLITE_OK) { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); goto fail; }
  sqlite3_bind_text(st, 1, entry_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, date, -1, SQLITE_TRANSIENT);
  if (memo) sqlite3_bind_text(st, 3, memo, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 3);
  sqlite3_bind_text(st, 4, status, -1, SQLITE_TRANSIENT);
  if (reverses_id) sqlite3_bind_text(st, 5, reverses_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
  sqlite3_bind_text(st, 6, mb_source_str(src), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, created, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, device, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 9, seq);
  sqlite3_bind_int64(st, 10, lamport);
  sqlite3_bind_text(st, 11, hash, -1, SQLITE_TRANSIENT);
  if (has_prev) sqlite3_bind_text(st, 12, prev_hash, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 12);
  if (sqlite3_step(st) != SQLITE_DONE) { e = MB_FAIL(MB_ERR_DB, "insert entry: %s", sqlite3_errmsg(mb_store_handle(s))); sqlite3_finalize(st); goto fail; }
  sqlite3_finalize(st);

  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO posting(id,entry_id,account_id,amount,memo,counterparty_id) VALUES(?,?,?,?,?,?);",
        -1, &st, NULL) != SQLITE_OK) { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); goto fail; }
  for (int i = 0; i < n; i++) {
    char pid[40];
    if ((e = mb_uuid(pid, sizeof pid)) != MB_OK) { sqlite3_finalize(st); goto fail; }
    sqlite3_bind_text(st, 1, pid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, entry_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, sorted[i].account_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, sorted[i].amount);
    if (sorted[i].memo) sqlite3_bind_text(st, 5, sorted[i].memo, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
    if (sorted[i].counterparty_id) sqlite3_bind_text(st, 6, sorted[i].counterparty_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);
    if (sqlite3_step(st) != SQLITE_DONE) { e = MB_FAIL(MB_ERR_DB, "insert posting: %s", sqlite3_errmsg(mb_store_handle(s))); sqlite3_finalize(st); goto fail; }
    sqlite3_reset(st);
  }
  sqlite3_finalize(st);

  if ((e = mb_store_meta_set(s, "last_hash", hash)) != MB_OK) goto fail;

  free(sorted);
  snprintf(id_out, 40, "%s", entry_id);
  return MB_OK;

fail:
  free(sorted);
  return e;
}

/* post within the caller's transaction (for invoice/bill/payment composition) */
mb_err mb_journal_post_tx(mb_store *s, const char *date, const char *memo, mb_source src,
                          const mb_posting_in *postings, int n, char entry_id_out[40]) {
  return post_core(s, date, memo, src, "POSTED", NULL, postings, n, entry_id_out);
}

mb_err mb_journal_post(mb_store *s, const char *date, const char *memo, mb_source src,
                       const mb_posting_in *postings, int n, char entry_id_out[40]) {
  MB_TRY(mb_store_begin(s));
  mb_err e = post_core(s, date, memo, src, "POSTED", NULL, postings, n, entry_id_out);
  if (e != MB_OK) { mb_store_rollback(s); return e; }
  return mb_store_commit(s);
}

mb_err mb_journal_reverse(mb_store *s, const char *entry_id, char reversal_id_out[40]) {
  /* original must exist */
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT date FROM journal_entry WHERE id=?;",
                         -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, entry_id, -1, SQLITE_TRANSIENT);
  char date[24];
  int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) { sqlite3_finalize(st); return MB_FAIL(MB_ERR_NOT_FOUND, "entry %s", entry_id); }
  snprintf(date, sizeof date, "%s", (const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);

  /* load postings */
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT account_id, amount, memo, counterparty_id FROM posting WHERE entry_id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, entry_id, -1, SQLITE_TRANSIENT);

  int cap = 8, n = 0;
  mb_posting_in *ps = malloc((size_t)cap * sizeof *ps);
  char (*ids)[40] = malloc((size_t)cap * sizeof *ids);   /* own the account_id strings */
  char (*cps)[40] = malloc((size_t)cap * sizeof *cps);   /* own the counterparty_id strings ("" => NULL) */
  if (!ps || !ids || !cps) { free(ps); free(ids); free(cps); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (n == cap) {
      cap *= 2;
      mb_posting_in *np = realloc(ps, (size_t)cap * sizeof *ps);
      char (*ni)[40] = realloc(ids, (size_t)cap * sizeof *ni);
      char (*nc)[40] = realloc(cps, (size_t)cap * sizeof *nc);
      if (!np || !ni || !nc) { free(np ? np : ps); free(ni ? (void*)ni : (void*)ids); free(nc ? (void*)nc : (void*)cps); sqlite3_finalize(st); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
      ps = np; ids = ni; cps = nc;
    }
    snprintf(ids[n], 40, "%s", (const char *)sqlite3_column_text(st, 0));
    const char *cpc = (const char *)sqlite3_column_text(st, 3);
    snprintf(cps[n], 40, "%s", cpc ? cpc : "");
    ps[n].account_id = ids[n];
    ps[n].amount = -(mb_money)sqlite3_column_int64(st, 1);  /* negate */
    ps[n].memo = NULL;
    ps[n].counterparty_id = cps[n][0] ? cps[n] : NULL;
    n++;
  }
  sqlite3_finalize(st);

  char memo[80];
  snprintf(memo, sizeof memo, "Reversal of %.40s", entry_id);
  mb_err e = mb_store_begin(s);
  if (e == MB_OK) {
    e = post_core(s, date, memo, MB_SRC_USER, "REVERSAL", entry_id, ps, n, reversal_id_out);
    /* Mark the original cancelled so status-filtered effective views (e.g. category_txns) exclude
     * it. Postings stay immutable; only this lifecycle flag changes, and it is not part of the
     * content hash, so the tamper-evident chain is unaffected. */
    if (e == MB_OK) {
      sqlite3_stmt *u;
      if (sqlite3_prepare_v2(mb_store_handle(s),
            "UPDATE journal_entry SET status='REVERSED' WHERE id=?;", -1, &u, NULL) == SQLITE_OK) {
        sqlite3_bind_text(u, 1, entry_id, -1, SQLITE_TRANSIENT);
        e = (sqlite3_step(u) == SQLITE_DONE) ? MB_OK
            : MB_FAIL(MB_ERR_DB, "mark reversed: %s", sqlite3_errmsg(mb_store_handle(s)));
        sqlite3_finalize(u);
      } else {
        e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
      }
    }
    if (e != MB_OK) mb_store_rollback(s); else e = mb_store_commit(s);
  }
  free(ps);
  free(ids);
  free(cps);
  return e;
}

mb_err mb_journal_is_reversed(mb_store *s, const char *entry_id, int *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT EXISTS(SELECT 1 FROM journal_entry WHERE reverses_id=?);", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, entry_id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_ROW) ? (*out = sqlite3_column_int(st, 0), MB_OK)
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_account_balance(mb_store *s, const char *account_id, mb_money *out) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "SELECT COALESCE(SUM(amount),0) FROM posting WHERE account_id=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, account_id, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_ROW) ? (*out = (mb_money)sqlite3_column_int64(st, 0), MB_OK)
             : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../account/account.h"

static void seed2(mb_store *s, char cash[40], char income[40]) {
  mb_account_new a = {.name = "Cash", .type = MB_ACCT_ASSET, .role = MB_ROLE_ACCOUNT};
  mb_account_new b = {.name = "Consulting Income", .type = MB_ACCT_INCOME, .role = MB_ROLE_CATEGORY};
  (void)mb_account_create(s, &a, cash);
  (void)mb_account_create(s, &b, income);
}

TEST(journal, post_balanced) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], income[40]; seed2(s, cash, income);
  mb_posting_in p[] = {
    {.account_id = cash,   .amount = 10000},   /* Dr Cash 100 */
    {.account_id = income, .amount = -10000},  /* Cr Income 100 */
  };
  char eid[40];
  ASSERT_OK(mb_journal_post(s, "2026-06-14", "Invoice payment", MB_SRC_USER, p, 2, eid));
  mb_money bal;
  ASSERT_OK(mb_account_balance(s, cash, &bal));   ASSERT_MONEY_EQ(bal, 10000);
  ASSERT_OK(mb_account_balance(s, income, &bal)); ASSERT_MONEY_EQ(bal, -10000);
  mb_store_close(s);
}

TEST(journal, rejects_unbalanced) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], income[40]; seed2(s, cash, income);
  mb_posting_in p[] = {
    {.account_id = cash,   .amount = 10000},
    {.account_id = income, .amount = -9999},   /* off by a penny */
  };
  char eid[40];
  ASSERT_ERR(mb_journal_post(s, "2026-06-14", "bad", MB_SRC_USER, p, 2, eid), MB_ERR_UNBALANCED);
  mb_money bal;
  ASSERT_OK(mb_account_balance(s, cash, &bal)); ASSERT_MONEY_EQ(bal, 0);  /* nothing committed */
  mb_store_close(s);
}

TEST(journal, rejects_missing_account) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], income[40]; seed2(s, cash, income);
  mb_posting_in p[] = {
    {.account_id = cash,        .amount = 10000},
    {.account_id = "ghost-acct",.amount = -10000},
  };
  char eid[40];
  ASSERT_ERR(mb_journal_post(s, "2026-06-14", "x", MB_SRC_USER, p, 2, eid), MB_ERR_NOT_FOUND);
  mb_store_close(s);
}

TEST(journal, reverse_nets_to_zero) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], income[40]; seed2(s, cash, income);
  mb_posting_in p[] = {
    {.account_id = cash,   .amount = 10000},
    {.account_id = income, .amount = -10000},
  };
  char eid[40], rid[40];
  ASSERT_OK(mb_journal_post(s, "2026-06-14", "payment", MB_SRC_USER, p, 2, eid));
  int reversed = 0;
  ASSERT_OK(mb_journal_is_reversed(s, eid, &reversed)); ASSERT_EQ_INT(reversed, 0);
  ASSERT_OK(mb_journal_reverse(s, eid, rid));
  ASSERT_OK(mb_journal_is_reversed(s, eid, &reversed)); ASSERT_EQ_INT(reversed, 1);
  mb_money bal;
  ASSERT_OK(mb_account_balance(s, cash, &bal));   ASSERT_MONEY_EQ(bal, 0);
  ASSERT_OK(mb_account_balance(s, income, &bal)); ASSERT_MONEY_EQ(bal, 0);
  mb_store_close(s);
}

TEST(journal, hash_chain_links) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], income[40]; seed2(s, cash, income);
  mb_posting_in p[] = {{.account_id = cash, .amount = 5000}, {.account_id = income, .amount = -5000}};
  char e1[40], e2[40];
  ASSERT_OK(mb_journal_post(s, "2026-06-14", "one", MB_SRC_USER, p, 2, e1));
  ASSERT_OK(mb_journal_post(s, "2026-06-14", "two", MB_SRC_USER, p, 2, e2));
  /* entry 2's prev_hash must equal entry 1's content_hash */
  sqlite3_stmt *st;
  sqlite3_prepare_v2(mb_store_handle(s), "SELECT content_hash FROM journal_entry WHERE id=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, e1, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  char h1[65]; snprintf(h1, sizeof h1, "%s", (const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  sqlite3_prepare_v2(mb_store_handle(s), "SELECT prev_hash FROM journal_entry WHERE id=?;", -1, &st, NULL);
  sqlite3_bind_text(st, 1, e2, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  char p2[65]; snprintf(p2, sizeof p2, "%s", (const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  ASSERT_STR_EQ(p2, h1);
  ASSERT_EQ_INT((long)strlen(h1), 64);
  mb_store_close(s);
}
#endif
