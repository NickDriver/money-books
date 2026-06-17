#ifndef MB_REDACT_H
#define MB_REDACT_H
/*
 * Money Books — redaction / pseudonymization (Phase 5, D10/D11).
 *
 * The egress choke point: before any text leaves for a cloud LLM, swap sensitive
 * identifiers for stable tokens; on the way back, swap them home. Sensitive by default
 * (D11): counterparty names (Client_N / Vendor_N) and money-account names (Account_N).
 * Category/system account names pass through (structural, not identifying), and amounts
 * pass through (the agent needs them to compute).
 */
#include "../store/store.h"

typedef struct mb_redactor mb_redactor;

/* Build a redactor from the book's current counterparties + money accounts. */
mb_err mb_redactor_create(mb_store *s, mb_redactor **out) MB_MUST_CHECK;
void   mb_redactor_free(mb_redactor *r);

/* Replace sensitive names with tokens. Returns a malloc'd string (free with free()). */
char *mb_redact(mb_redactor *r, const char *in);
/* Replace tokens back with the real names. Returns a malloc'd string (free with free()). */
char *mb_restore(mb_redactor *r, const char *in);

int mb_redactor_count(const mb_redactor *r);  /* number of mapped entities (for tests) */

#endif /* MB_REDACT_H */
