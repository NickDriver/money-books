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

/* Canonical content hash over an entry's identity + its sorted postings. The exact byte
 * stream here defines entry identity across devices — local posts and replayed remote
 * entries MUST produce it identically, so both paths call this one function. */
static void compute_content_hash(const char *date, const char *memo, const char *src_str,
                                 const char *device, int64_t seq, int64_t lamport,
                                 const char *prev_hash, const mb_posting_in *sorted, int n,
                                 char out[65]) {
  mb_sha256 h;
  mb_sha256_init(&h);
  char line[256];
  int m = snprintf(line, sizeof line, "%s\n%s\n%s\n%s\n%lld\n%lld\n%s\n",
                   date, memo ? memo : "", src_str, device,
                   (long long)seq, (long long)lamport, prev_hash ? prev_hash : "");
  mb_sha256_update(&h, line, (size_t)m);
  for (int i = 0; i < n; i++) {
    m = snprintf(line, sizeof line, "%s\t%lld\t%s\n", sorted[i].account_id,
                 (long long)sorted[i].amount, sorted[i].memo ? sorted[i].memo : "");
    mb_sha256_update(&h, line, (size_t)m);
  }
  mb_sha256_final_hex(&h, out);
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
  char hkey[56];
  int has_prev = 1;
  if ((e = mb_store_next_stamp(s, &seq, &lamport)) != MB_OK) goto fail;
  if ((e = mb_store_device_id(s, device)) != MB_OK) goto fail;
  /* prev_hash chains within this device's own log (last_hash:<device_id>) so concurrent
   * appends on other devices don't fork the chain (Phase 7a-0, docs/PHASE7_SYNC.md §3). */
  snprintf(hkey, sizeof hkey, "last_hash:%s", device);
  {
    mb_err pe = mb_store_meta_get(s, hkey, prev_hash, sizeof prev_hash);
    if (pe == MB_ERR_NOT_FOUND) { has_prev = 0; prev_hash[0] = '\0'; }
    else if (pe != MB_OK) { e = pe; goto fail; }
  }

  /* content hash over canonical serialization */
  char hash[65];
  compute_content_hash(date, memo, mb_source_str(src), device, seq, lamport, prev_hash, sorted, n, hash);

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

  if ((e = mb_store_meta_set(s, hkey, hash)) != MB_OK) goto fail;

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

/* Replay a foreign entry verbatim. Assumes the caller holds a transaction. */
mb_err mb_journal_apply_remote_tx(mb_store *s, const mb_remote_entry *r) {
  if (!r || !r->id || !r->date || !r->date[0] || !r->device_id || !r->content_hash)
    return MB_FAIL(MB_ERR_INVALID_ARG, "incomplete remote entry");
  if (r->n < 2) return MB_FAIL(MB_ERR_INVALID_ARG, "an entry needs >= 2 postings");

  /* idempotent: already hold this entry (by content hash or id)? -> no-op */
  {
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(mb_store_handle(s),
          "SELECT 1 FROM journal_entry WHERE content_hash=? OR id=?;", -1, &q, NULL) != SQLITE_OK)
      return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
    sqlite3_bind_text(q, 1, r->content_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, r->id, -1, SQLITE_TRANSIENT);
    int have = (sqlite3_step(q) == SQLITE_ROW);
    sqlite3_finalize(q);
    if (have) return MB_OK;
  }

  /* re-validate the invariants the engine guarantees: balanced + real accounts.
   * A peer cannot inject an unbalanced or dangling entry. */
  mb_money sum = 0;
  for (int i = 0; i < r->n; i++) MB_TRY(mb_money_add(sum, r->postings[i].amount, &sum));
  if (sum != 0) return MB_FAIL(MB_ERR_UNBALANCED, "remote postings sum to %lld, not 0", (long long)sum);
  for (int i = 0; i < r->n; i++) MB_TRY(check_account(s, r->postings[i].account_id));

  /* recompute the content hash over the canonical (sorted) form and verify it */
  mb_posting_in *sorted = malloc((size_t)r->n * sizeof *sorted);
  if (!sorted) return MB_FAIL(MB_ERR_INTERNAL, "oom");
  memcpy(sorted, r->postings, (size_t)r->n * sizeof *sorted);
  qsort(sorted, (size_t)r->n, sizeof *sorted, cmp_posting);

  char hash[65];
  compute_content_hash(r->date, r->memo, mb_source_str(r->src), r->device_id,
                       r->seq, r->lamport, r->prev_hash, sorted, r->n, hash);
  mb_err e = MB_OK;
  if (strcmp(hash, r->content_hash) != 0) {
    e = MB_FAIL(MB_ERR_INVALID_ARG, "content hash mismatch for remote entry %.8s", r->id);
    goto done;
  }

  /* chain check: prev_hash must equal the content_hash of (device_id, seq-1), which must
   * already be present (entries arrive in per-device seq order). Genesis carries no prev. */
  if (r->prev_hash && r->prev_hash[0]) {
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(mb_store_handle(s),
          "SELECT content_hash FROM journal_entry WHERE device_id=? AND seq=?;", -1, &q, NULL) != SQLITE_OK)
      { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); goto done; }
    sqlite3_bind_text(q, 1, r->device_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 2, r->seq - 1);
    int rc = sqlite3_step(q);
    char prev[65] = "";
    if (rc == SQLITE_ROW) snprintf(prev, sizeof prev, "%s", (const char *)sqlite3_column_text(q, 0));
    sqlite3_finalize(q);
    if (rc != SQLITE_ROW) {
      e = MB_FAIL(MB_ERR_NOT_FOUND, "predecessor (%.8s, seq %lld) absent", r->device_id, (long long)(r->seq - 1));
      goto done;
    }
    if (strcmp(prev, r->prev_hash) != 0) {
      e = MB_FAIL(MB_ERR_INVALID_ARG, "broken chain at %.8s seq %lld", r->device_id, (long long)r->seq);
      goto done;
    }
  }

  /* insert preserving the foreign identity — no new id/seq/lamport/hash is minted */
  char created[24];
  if (!r->created_at || !r->created_at[0]) mb_now_iso(created, sizeof created);
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO journal_entry(id,date,memo,status,reverses_id,source,created_at,"
        "device_id,seq,lamport,content_hash,prev_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?,?);",
        -1, &st, NULL) != SQLITE_OK) { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); goto done; }
  sqlite3_bind_text(st, 1, r->id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, r->date, -1, SQLITE_TRANSIENT);
  if (r->memo) sqlite3_bind_text(st, 3, r->memo, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 3);
  sqlite3_bind_text(st, 4, (r->status && r->status[0]) ? r->status : "POSTED", -1, SQLITE_TRANSIENT);
  if (r->reverses_id && r->reverses_id[0]) sqlite3_bind_text(st, 5, r->reverses_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
  sqlite3_bind_text(st, 6, mb_source_str(r->src), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, (r->created_at && r->created_at[0]) ? r->created_at : created, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, r->device_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 9, r->seq);
  sqlite3_bind_int64(st, 10, r->lamport);
  sqlite3_bind_text(st, 11, r->content_hash, -1, SQLITE_TRANSIENT);
  if (r->prev_hash && r->prev_hash[0]) sqlite3_bind_text(st, 12, r->prev_hash, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 12);
  if (sqlite3_step(st) != SQLITE_DONE) { e = MB_FAIL(MB_ERR_DB, "insert remote entry: %s", sqlite3_errmsg(mb_store_handle(s))); sqlite3_finalize(st); goto done; }
  sqlite3_finalize(st);

  /* postings: mint local posting ids (posting id is not part of entry identity and is never
   * referenced across devices), but preserve account/amount/memo/counterparty verbatim. */
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO posting(id,entry_id,account_id,amount,memo,counterparty_id) VALUES(?,?,?,?,?,?);",
        -1, &st, NULL) != SQLITE_OK) { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); goto done; }
  for (int i = 0; i < r->n; i++) {
    char pid[40];
    if ((e = mb_uuid(pid, sizeof pid)) != MB_OK) { sqlite3_finalize(st); goto done; }
    sqlite3_bind_text(st, 1, pid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, r->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, r->postings[i].account_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, r->postings[i].amount);
    if (r->postings[i].memo) sqlite3_bind_text(st, 5, r->postings[i].memo, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
    if (r->postings[i].counterparty_id) sqlite3_bind_text(st, 6, r->postings[i].counterparty_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);
    if (sqlite3_step(st) != SQLITE_DONE) { e = MB_FAIL(MB_ERR_DB, "insert remote posting: %s", sqlite3_errmsg(mb_store_handle(s))); sqlite3_finalize(st); goto done; }
    sqlite3_reset(st);
  }
  sqlite3_finalize(st);

  /* REVERSED is derived, not synced: applying a REVERSAL flips its target's lifecycle flag
   * (mirrors mb_journal_reverse). status is outside the content hash, so this is safe. */
  if (r->reverses_id && r->reverses_id[0]) {
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(mb_store_handle(s),
          "UPDATE journal_entry SET status='REVERSED' WHERE id=?;", -1, &u, NULL) != SQLITE_OK)
      { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); goto done; }
    sqlite3_bind_text(u, 1, r->reverses_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(u) != SQLITE_DONE) e = MB_FAIL(MB_ERR_DB, "mark reversed: %s", sqlite3_errmsg(mb_store_handle(s)));
    sqlite3_finalize(u);
    if (e != MB_OK) goto done;
  }

  /* advance the book-wide lamport so future local entries sort after this merged one */
  {
    int64_t lam = 0;
    mb_err le = mb_store_meta_get_int(s, "lamport", &lam);
    if (le != MB_OK && le != MB_ERR_NOT_FOUND) { e = le; goto done; }
    if (r->lamport > lam) e = mb_store_meta_set_int(s, "lamport", r->lamport);
  }

done:
  free(sorted);
  return e;
}

mb_err mb_journal_apply_remote(mb_store *s, const mb_remote_entry *r) {
  MB_TRY(mb_store_begin(s));
  mb_err e = mb_journal_apply_remote_tx(s, r);
  if (e != MB_OK) { mb_store_rollback(s); return e; }
  return mb_store_commit(s);
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

/* Build a self-consistent foreign entry: compute its content_hash exactly as the engine
 * would, so apply_remote's verification passes unless we deliberately tamper afterwards. */
static void mk_remote(mb_remote_entry *r, const char *dev, int64_t seq, int64_t lamport,
                      const char *prev, mb_posting_in *p, int n, char id_out[40], char hash_out[65]) {
  (void)mb_uuid(id_out, 40);
  mb_posting_in tmp[16];
  memcpy(tmp, p, (size_t)n * sizeof *tmp);
  qsort(tmp, (size_t)n, sizeof *tmp, cmp_posting);
  compute_content_hash("2026-06-15", "remote", "USER", dev, seq, lamport, prev, tmp, n, hash_out);
  r->id = id_out; r->date = "2026-06-15"; r->memo = "remote"; r->status = "POSTED";
  r->reverses_id = NULL; r->src = MB_SRC_USER; r->created_at = NULL; r->device_id = dev;
  r->seq = seq; r->lamport = lamport; r->content_hash = hash_out; r->prev_hash = prev;
  r->postings = p; r->n = n;
}

TEST(journal, per_device_log) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], income[40]; seed2(s, cash, income);
  /* a local entry — this book's device, its seq 1 */
  mb_posting_in lp[] = {{.account_id = cash, .amount = 7000}, {.account_id = income, .amount = -7000}};
  char le[40]; ASSERT_OK(mb_journal_post(s, "2026-06-15", "local", MB_SRC_USER, lp, 2, le));
  /* a remote entry from DEVICE-B — its own independent log, seq 1, genesis (no prev) */
  mb_posting_in rp[] = {{.account_id = cash, .amount = 3000}, {.account_id = income, .amount = -3000}};
  mb_remote_entry r; char rid[40], rhash[65];
  mk_remote(&r, "DEVICE-B", 1, 9, "", rp, 2, rid, rhash);
  ASSERT_OK(mb_journal_apply_remote(s, &r));

  mb_money bal; ASSERT_OK(mb_account_balance(s, cash, &bal)); ASSERT_MONEY_EQ(bal, 10000);

  /* two independent per-device logs now coexist. DEVICE-B's first-ever entry is its own
   * seq 1 even though this book's shared counter was already past seq 1 (the seed accounts
   * consumed it) — proving the seq counter is per-device, not book-global. */
  sqlite3_stmt *q;
  sqlite3_prepare_v2(mb_store_handle(s),
    "SELECT COUNT(DISTINCT device_id) FROM journal_entry;", -1, &q, NULL);
  ASSERT_EQ_INT(sqlite3_step(q), SQLITE_ROW);
  ASSERT_EQ_INT(sqlite3_column_int(q, 0), 2);
  sqlite3_finalize(q);
  sqlite3_prepare_v2(mb_store_handle(s),
    "SELECT seq FROM journal_entry WHERE device_id='DEVICE-B';", -1, &q, NULL);
  ASSERT_EQ_INT(sqlite3_step(q), SQLITE_ROW);
  ASSERT_EQ_INT(sqlite3_column_int(q, 0), 1);
  sqlite3_finalize(q);

  /* the remote lamport (9) is observed: the next local entry sorts strictly after it */
  char le2[40]; ASSERT_OK(mb_journal_post(s, "2026-06-15", "after", MB_SRC_USER, lp, 2, le2));
  int64_t lam; ASSERT_OK(mb_store_meta_get_int(s, "lamport", &lam));
  ASSERT_EQ_INT(lam, 10);
  mb_store_close(s);
}

TEST(journal, apply_remote_idempotent_and_verified) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], income[40]; seed2(s, cash, income);
  mb_posting_in rp[] = {{.account_id = cash, .amount = 4000}, {.account_id = income, .amount = -4000}};
  mb_remote_entry r; char rid[40], rhash[65];
  mk_remote(&r, "DEVICE-B", 1, 5, "", rp, 2, rid, rhash);
  ASSERT_OK(mb_journal_apply_remote(s, &r));
  ASSERT_OK(mb_journal_apply_remote(s, &r));   /* re-apply is a no-op, not a double-post */
  mb_money bal; ASSERT_OK(mb_account_balance(s, cash, &bal)); ASSERT_MONEY_EQ(bal, 4000);

  /* tamper: payload altered after the hash was computed -> rejected */
  mb_posting_in p6[] = {{.account_id = cash, .amount = 6000}, {.account_id = income, .amount = -6000}};
  mb_remote_entry tam; char tid[40], thash[65];
  mk_remote(&tam, "DEVICE-C", 1, 7, "", p6, 2, tid, thash);
  mb_posting_in p5[] = {{.account_id = cash, .amount = 5000}, {.account_id = income, .amount = -5000}};
  tam.postings = p5;   /* hash no longer matches the postings */
  ASSERT_ERR(mb_journal_apply_remote(s, &tam), MB_ERR_INVALID_ARG);

  /* an unbalanced remote entry is refused regardless of its hash */
  mb_posting_in pu[] = {{.account_id = cash, .amount = 5000}, {.account_id = income, .amount = -4000}};
  mb_remote_entry u; char uid[40], uhash[65];
  mk_remote(&u, "DEVICE-D", 1, 8, "", pu, 2, uid, uhash);
  ASSERT_ERR(mb_journal_apply_remote(s, &u), MB_ERR_UNBALANCED);
  mb_store_close(s);
}

TEST(journal, apply_remote_chain) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cash[40], income[40]; seed2(s, cash, income);
  mb_posting_in p1[] = {{.account_id = cash, .amount = 1000}, {.account_id = income, .amount = -1000}};
  mb_remote_entry r1; char id1[40], h1[65];
  mk_remote(&r1, "DEVICE-B", 1, 3, "", p1, 2, id1, h1);
  ASSERT_OK(mb_journal_apply_remote(s, &r1));

  /* seq 2 chained off h1 links cleanly */
  mb_posting_in p2[] = {{.account_id = cash, .amount = 2000}, {.account_id = income, .amount = -2000}};
  mb_remote_entry r2; char id2[40], h2[65];
  mk_remote(&r2, "DEVICE-B", 2, 4, h1, p2, 2, id2, h2);
  ASSERT_OK(mb_journal_apply_remote(s, &r2));

  /* seq 3 pointing at the wrong predecessor is rejected */
  mb_posting_in p3[] = {{.account_id = cash, .amount = 3000}, {.account_id = income, .amount = -3000}};
  mb_remote_entry r3; char id3[40], h3[65];
  mk_remote(&r3, "DEVICE-B", 3, 5, "deadbeef", p3, 2, id3, h3);
  ASSERT_ERR(mb_journal_apply_remote(s, &r3), MB_ERR_INVALID_ARG);

  /* seq 5 whose predecessor (seq 4) hasn't arrived is rejected (apply in order) */
  mb_posting_in p5[] = {{.account_id = cash, .amount = 5000}, {.account_id = income, .amount = -5000}};
  mb_remote_entry r5; char id5[40], h5[65];
  mk_remote(&r5, "DEVICE-B", 5, 6, h2, p5, 2, id5, h5);
  ASSERT_ERR(mb_journal_apply_remote(s, &r5), MB_ERR_NOT_FOUND);
  mb_store_close(s);
}
#endif
