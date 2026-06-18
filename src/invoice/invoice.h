#ifndef MB_INVOICE_H
#define MB_INVOICE_H
/*
 * Money Books — invoices (Phase 2, SPEC §4.3, D4/D5/D15).
 * Lifecycle: DRAFT (editable, posts nothing) -> issue -> OPEN (posts Dr AR / Cr Income[/Tax]).
 * Corrections after issue are via journal reversal (D13).
 */
#include "../store/store.h"
#include "../money/money.h"

typedef enum { MB_INV_DRAFT, MB_INV_OPEN, MB_INV_PARTIAL, MB_INV_PAID, MB_INV_VOID } mb_invoice_status;
const char *mb_invoice_status_str(mb_invoice_status st);

typedef struct {
  char              id[40];
  char              number[40];
  char              counterparty_id[40];
  char              issue_date[24];   /* "" until issued */
  char              due_date[24];
  mb_invoice_status status;
  char              memo[256];
  char              currency[8];
  char              entry_id[40];     /* "" until issued */
} mb_invoice;

typedef struct {
  const char *item_id;      /* nullable */
  const char *description;  /* required */
  int64_t     qty_centi;    /* quantity x100; <=0 => 100 (i.e. 1) */
  mb_money    unit_price;   /* cents */
  const char *account_id;   /* income account, or the tax-payable account for a tax line */
  int         is_tax;
} mb_invoice_line_in;

mb_err mb_invoice_create(mb_store *s, const char *counterparty_id, const char *number,
                         const char *due_date, const char *memo, char id_out[40]) MB_MUST_CHECK;
mb_err mb_invoice_add_line(mb_store *s, const char *invoice_id, const mb_invoice_line_in *in,
                           char line_id_out[40]) MB_MUST_CHECK;
mb_err mb_invoice_total(mb_store *s, const char *invoice_id, mb_money *out) MB_MUST_CHECK;
mb_err mb_invoice_issue(mb_store *s, const char *invoice_id, const char *issue_date) MB_MUST_CHECK;
mb_err mb_invoice_get(mb_store *s, const char *id, mb_invoice *out) MB_MUST_CHECK;

/* A row for the list view: invoice header + counterparty name + computed total. */
typedef struct {
  char              id[40];
  char              number[40];
  char              counterparty_id[40];
  char              counterparty_name[128];
  char              issue_date[24];
  char              due_date[24];
  mb_invoice_status status;
  mb_money          total;
} mb_invoice_row;

/* Newest first. Caller frees *rows_out. */
mb_err mb_invoice_list(mb_store *s, mb_invoice_row **rows_out, int *n_out) MB_MUST_CHECK;

/* A line of an invoice, joined with its account name, for the detail view. */
typedef struct {
  char     id[40];
  char     description[256];
  int64_t  qty_centi;
  mb_money unit_price;
  mb_money line_total;
  char     account_id[40];
  char     account_name[128];
  int      is_tax;
} mb_invoice_line;

mb_err mb_invoice_lines(mb_store *s, const char *invoice_id, mb_invoice_line **rows, int *n) MB_MUST_CHECK;

/* Editing (D13 — a document is editable only while DRAFT). Once issued it is locked; corrections
 * are made with new documents (a credit note / next invoice) or by voiding. */
mb_err mb_invoice_remove_line(mb_store *s, const char *line_id) MB_MUST_CHECK;
mb_err mb_invoice_update(mb_store *s, const char *id, const char *number,
                         const char *due_date, const char *memo) MB_MUST_CHECK;

/* Void an issued, UNPAID (OPEN) invoice: posts a reversing entry that cancels AR + income, marks the
 * document VOID, and drops it from AR aging. Rejected for DRAFT (not issued) and PARTIAL/PAID
 * (has cash applied — use a refund or credit note instead). */
mb_err mb_invoice_void(mb_store *s, const char *id) MB_MUST_CHECK;

#endif /* MB_INVOICE_H */
