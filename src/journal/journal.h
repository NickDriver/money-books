#ifndef MB_JOURNAL_H
#define MB_JOURNAL_H
/*
 * Money Books — journal: balanced, immutable double-entry postings (Phase 1, SPEC §4.1/§5).
 * Entries never change; corrections are reversing entries (D13). Each entry carries sync
 * identity + a tamper-evident hash chain (D20).
 */
#include "../store/store.h"
#include "../money/money.h"

typedef enum { MB_SRC_USER, MB_SRC_AI, MB_SRC_IMPORT } mb_source;
const char *mb_source_str(mb_source src);

typedef struct {
  const char *account_id;
  mb_money    amount;          /* signed cents: + debit, - credit */
  const char *memo;            /* nullable */
  const char *counterparty_id; /* nullable; set on AR/AP control postings for per-party balances (D26) */
} mb_posting_in;

/* Post a balanced entry (>=2 postings, Σ amount == 0). All accounts must exist & be active. */
mb_err mb_journal_post(mb_store *s, const char *date, const char *memo, mb_source src,
                       const mb_posting_in *postings, int n, char entry_id_out[40]) MB_MUST_CHECK;

/* Same, but assumes the caller already holds a transaction (for composing with other writes). */
mb_err mb_journal_post_tx(mb_store *s, const char *date, const char *memo, mb_source src,
                          const mb_posting_in *postings, int n, char entry_id_out[40]) MB_MUST_CHECK;

/* Post a reversing entry (negated postings) that cancels entry_id. */
mb_err mb_journal_reverse(mb_store *s, const char *entry_id, char reversal_id_out[40]) MB_MUST_CHECK;

/*
 * A journal entry received from another device, to be replayed verbatim (Phase 7 sync).
 * Unlike mb_journal_post, this preserves the originating identity (device_id, seq, lamport,
 * content_hash, prev_hash) rather than minting fresh ones — sync is a set-union of immutable
 * logs, not a re-post. The receiver re-validates (balance, accounts, content hash, hash chain)
 * before inserting; a tampered or unbalanced entry is rejected.
 */
typedef struct {
  const char *id;            /* entry uuid, preserved */
  const char *date;
  const char *memo;          /* nullable */
  const char *status;        /* nullable -> "POSTED"; one of POSTED/REVERSED/REVERSAL */
  const char *reverses_id;   /* nullable; set on REVERSAL entries */
  mb_source   src;
  const char *created_at;    /* nullable -> now */
  const char *device_id;     /* originating device */
  int64_t     seq;           /* per-device sequence */
  int64_t     lamport;       /* originating lamport clock */
  const char *content_hash;  /* must recompute to this */
  const char *prev_hash;     /* nullable/"" at the device's genesis entry */
  const mb_posting_in *postings;
  int         n;
} mb_remote_entry;

/* Verify + apply a foreign entry. Idempotent: re-applying a known entry is a no-op (MB_OK). */
mb_err mb_journal_apply_remote(mb_store *s, const mb_remote_entry *e) MB_MUST_CHECK;
/* Same, but assumes the caller already holds a transaction (for batch merges). */
mb_err mb_journal_apply_remote_tx(mb_store *s, const mb_remote_entry *e) MB_MUST_CHECK;

/* True if some REVERSAL entry points at entry_id. */
mb_err mb_journal_is_reversed(mb_store *s, const char *entry_id, int *out) MB_MUST_CHECK;

/* Sum of postings for an account (signed cents). */
mb_err mb_account_balance(mb_store *s, const char *account_id, mb_money *out) MB_MUST_CHECK;

#endif /* MB_JOURNAL_H */
