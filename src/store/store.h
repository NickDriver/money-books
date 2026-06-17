#ifndef MB_STORE_H
#define MB_STORE_H
/*
 * Money Books — SQLite store (Phase 1).
 *
 * Owns the database connection, runs migrations (user_version), and provides
 * transaction + metadata + sync-stamp helpers. The engine is the only writer
 * (SPEC D8); all writes funnel through here, wrapped in BEGIN IMMEDIATE.
 */
#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>
#include "../support/mb_error.h"

typedef struct mb_store mb_store;

mb_err mb_store_open(const char *path, mb_store **out) MB_MUST_CHECK;
mb_err mb_store_open_memory(mb_store **out) MB_MUST_CHECK;  /* for tests */
void   mb_store_close(mb_store *s);

sqlite3 *mb_store_handle(mb_store *s);  /* for modules to prepare statements */

/* transactions (BEGIN IMMEDIATE — takes the write lock up front, RESEARCH §4) */
mb_err mb_store_begin(mb_store *s) MB_MUST_CHECK;
mb_err mb_store_commit(mb_store *s) MB_MUST_CHECK;
void   mb_store_rollback(mb_store *s);

mb_err mb_store_exec(mb_store *s, const char *sql) MB_MUST_CHECK;

/* book metadata */
mb_err mb_store_meta_get(mb_store *s, const char *key, char *buf, size_t buflen) MB_MUST_CHECK;
mb_err mb_store_meta_set(mb_store *s, const char *key, const char *val) MB_MUST_CHECK;
mb_err mb_store_meta_get_int(mb_store *s, const char *key, int64_t *out) MB_MUST_CHECK;
mb_err mb_store_meta_set_int(mb_store *s, const char *key, int64_t val) MB_MUST_CHECK;

/* allocate the next per-device sync stamp (D20). Caller MUST be inside a txn. */
mb_err mb_store_next_stamp(mb_store *s, int64_t *seq, int64_t *lamport) MB_MUST_CHECK;

mb_err mb_store_device_id(mb_store *s, char buf[40]) MB_MUST_CHECK;
mb_err mb_store_currency(mb_store *s, char buf[8]) MB_MUST_CHECK;

/* current UTC time as ISO-8601 ("2026-06-14T10:00:00Z"); buflen >= 21. */
void mb_now_iso(char *buf, size_t buflen);

#endif /* MB_STORE_H */
