#include "redact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *name; char *token; } pair;

struct mb_redactor {
  pair *pairs;
  int   n;
};

/* replace every occurrence of `find` in `in` with `repl`; malloc'd result. */
static char *replace_all(const char *in, const char *find, const char *repl) {
  size_t fl = strlen(find), rl = strlen(repl), il = strlen(in);
  if (fl == 0) { char *d = malloc(il + 1); if (d) memcpy(d, in, il + 1); return d; }
  size_t count = 0;
  for (const char *p = in; (p = strstr(p, find)); p += fl) count++;
  char *out = malloc(il + count * rl - count * fl + 1);
  if (!out) return NULL;
  char *o = out;
  const char *p = in, *q;
  while ((q = strstr(p, find))) {
    size_t seg = (size_t)(q - p);
    memcpy(o, p, seg); o += seg;
    memcpy(o, repl, rl); o += rl;
    p = q + fl;
  }
  strcpy(o, p);
  return out;
}

static int by_name_len_desc(const void *A, const void *B) {
  const pair *a = A, *b = B;
  size_t la = strlen(a->name), lb = strlen(b->name);
  return (la < lb) - (la > lb);
}
static int by_token_len_desc(const void *A, const void *B) {
  const pair *a = A, *b = B;
  size_t la = strlen(a->token), lb = strlen(b->token);
  return (la < lb) - (la > lb);
}

static mb_err add_pair(mb_redactor *r, int *cap, const char *name, const char *token) {
  if (!name || !name[0]) return MB_OK;          /* skip empty names */
  if (r->n == *cap) {
    *cap *= 2;
    pair *np = realloc(r->pairs, (size_t)*cap * sizeof *np);
    if (!np) return MB_FAIL(MB_ERR_INTERNAL, "oom");
    r->pairs = np;
  }
  r->pairs[r->n].name = strdup(name);
  r->pairs[r->n].token = strdup(token);
  if (!r->pairs[r->n].name || !r->pairs[r->n].token) return MB_FAIL(MB_ERR_INTERNAL, "oom");
  r->n++;
  return MB_OK;
}

mb_err mb_redactor_create(mb_store *s, mb_redactor **out) {
  mb_redactor *r = calloc(1, sizeof *r);
  if (!r) return MB_FAIL(MB_ERR_INTERNAL, "oom");
  int cap = 16;
  r->pairs = malloc((size_t)cap * sizeof *r->pairs);
  if (!r->pairs) { free(r); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }

  sqlite3 *db = mb_store_handle(s);
  sqlite3_stmt *st;
  mb_err e = MB_OK;

  /* counterparties -> Client_N (VENDOR -> Vendor_N) */
  if (sqlite3_prepare_v2(db, "SELECT name, kind FROM counterparty ORDER BY name;", -1, &st, NULL) == SQLITE_OK) {
    int ci = 0, vi = 0;
    while (sqlite3_step(st) == SQLITE_ROW && e == MB_OK) {
      const char *name = (const char *)sqlite3_column_text(st, 0);
      const char *kind = (const char *)sqlite3_column_text(st, 1);
      char tok[24];
      if (kind && !strcmp(kind, "VENDOR")) snprintf(tok, sizeof tok, "Vendor_%d", ++vi);
      else snprintf(tok, sizeof tok, "Client_%d", ++ci);
      e = add_pair(r, &cap, name, tok);
    }
    sqlite3_finalize(st);
  }
  /* money accounts (role=ACCOUNT) -> Account_N */
  if (e == MB_OK && sqlite3_prepare_v2(db, "SELECT name FROM account WHERE role='ACCOUNT' ORDER BY name;", -1, &st, NULL) == SQLITE_OK) {
    int ai = 0;
    while (sqlite3_step(st) == SQLITE_ROW && e == MB_OK) {
      char tok[24];
      snprintf(tok, sizeof tok, "Account_%d", ++ai);
      e = add_pair(r, &cap, (const char *)sqlite3_column_text(st, 0), tok);
    }
    sqlite3_finalize(st);
  }
  if (e != MB_OK) { mb_redactor_free(r); return e; }
  *out = r;
  return MB_OK;
}

void mb_redactor_free(mb_redactor *r) {
  if (!r) return;
  for (int i = 0; i < r->n; i++) { free(r->pairs[i].name); free(r->pairs[i].token); }
  free(r->pairs);
  free(r);
}

int mb_redactor_count(const mb_redactor *r) { return r->n; }

/* generic pass: replace `from`->`to` for every pair, longest key first */
static char *transform(mb_redactor *r, const char *in, int restore) {
  /* order so longer keys are replaced first (avoids prefix collisions) */
  qsort(r->pairs, (size_t)r->n, sizeof *r->pairs, restore ? by_token_len_desc : by_name_len_desc);
  char *cur = malloc(strlen(in) + 1);
  if (!cur) return NULL;
  strcpy(cur, in);
  for (int i = 0; i < r->n; i++) {
    const char *from = restore ? r->pairs[i].token : r->pairs[i].name;
    const char *to   = restore ? r->pairs[i].name : r->pairs[i].token;
    char *next = replace_all(cur, from, to);
    free(cur);
    if (!next) return NULL;
    cur = next;
  }
  return cur;
}

char *mb_redact(mb_redactor *r, const char *in)  { return transform(r, in, 0); }
char *mb_restore(mb_redactor *r, const char *in) { return transform(r, in, 1); }

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../account/account.h"
#include "../counterparty/counterparty.h"

TEST(redact, names_to_tokens_and_back) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cp[40], bank[40], cat[40];
  mb_counterparty_new c = {.name = "Acme Corp", .kind = MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cp));
  mb_account_new b = {.name = "Business Checking", .type = MB_ACCT_ASSET, .role = MB_ROLE_ACCOUNT};
  mb_account_new k = {.name = "Consulting Income", .type = MB_ACCT_INCOME, .role = MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &b, bank));
  ASSERT_OK(mb_account_create(s, &k, cat));

  mb_redactor *r = NULL;
  ASSERT_OK(mb_redactor_create(s, &r));
  ASSERT_EQ_INT(mb_redactor_count(r), 2);   /* Acme + Business Checking; NOT the category */

  const char *txt = "Invoice to Acme Corp deposited in Business Checking as Consulting Income";
  char *red = mb_redact(r, txt);
  /* sensitive names gone, category preserved */
  ASSERT(strstr(red, "Acme Corp") == NULL);
  ASSERT(strstr(red, "Business Checking") == NULL);
  ASSERT(strstr(red, "Client_1") != NULL);
  ASSERT(strstr(red, "Account_1") != NULL);
  ASSERT(strstr(red, "Consulting Income") != NULL);

  char *back = mb_restore(r, red);
  ASSERT_STR_EQ(back, txt);

  free(red); free(back);
  mb_redactor_free(r);
  mb_store_close(s);
}

TEST(redact, prefix_collision_safe) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  /* 11 counterparties so Client_1 is a prefix of Client_10/Client_11 */
  for (int i = 0; i < 11; i++) {
    char name[32], id[40];
    snprintf(name, sizeof name, "Customer Number %02d", i);
    mb_counterparty_new c = {.name = name, .kind = MB_CP_CUSTOMER};
    ASSERT_OK(mb_counterparty_create(s, &c, id));
  }
  mb_redactor *r = NULL;
  ASSERT_OK(mb_redactor_create(s, &r));
  const char *txt = "Customer Number 00 and Customer Number 10 are different";
  char *red = mb_redact(r, txt);
  char *back = mb_restore(r, red);
  ASSERT_STR_EQ(back, txt);   /* round-trips despite Client_1 / Client_10 prefix overlap */
  free(red); free(back);
  mb_redactor_free(r);
  mb_store_close(s);
}
#endif
