#ifndef MB_EXPORT_H
#define MB_EXPORT_H
/*
 * Money Books — report export (Phase 7, accountant sharing).
 *
 * Renders the existing report views (src/report) as accountant-friendly files. CSV first:
 * pure C, no dependencies, opens cleanly in Excel/Sheets and imports into accounting tools.
 * These run off the same read-only report layer the UI uses, so they are equally available
 * to a local "Download" action and (later) a remote read-only guest session.
 *
 * Each function allocates a NUL-terminated document; the caller frees it with free().
 * Date windows are inclusive; NULL = open end (matching src/report). Amounts are decimal
 * strings (mb_money_format); fields are RFC-4180 escaped; rows end with "\n".
 */
#include "../store/store.h"

mb_err mb_export_trial_balance_csv(mb_store *s, const char *as_of, char **out) MB_MUST_CHECK;
mb_err mb_export_pnl_csv(mb_store *s, const char *from, const char *to, char **out) MB_MUST_CHECK;
mb_err mb_export_balance_sheet_csv(mb_store *s, const char *as_of, char **out) MB_MUST_CHECK;
/* General ledger: every account with activity in the window, each posting with a running
 * balance. Flat (account columns repeated per row) so it pivots cleanly in a spreadsheet. */
mb_err mb_export_general_ledger_csv(mb_store *s, const char *from, const char *to, char **out) MB_MUST_CHECK;
/* Journal/transaction listing (the audit view): every entry incl. reversals. */
mb_err mb_export_journal_csv(mb_store *s, const char *from, const char *to, char **out) MB_MUST_CHECK;

#endif /* MB_EXPORT_H */
