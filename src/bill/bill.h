#ifndef MB_BILL_H
#define MB_BILL_H
/*
 * Money Books — bills (Phase 2, SPEC §4.3). Symmetric to invoices for the payable side.
 * DRAFT -> enter -> OPEN (posts Dr Expense / Cr Accounts Payable). Corrections via reversal.
 */
#include "../store/store.h"
#include "../money/money.h"

typedef enum { MB_BILL_DRAFT, MB_BILL_OPEN, MB_BILL_PARTIAL, MB_BILL_PAID, MB_BILL_VOID } mb_bill_status;
const char *mb_bill_status_str(mb_bill_status st);

typedef struct {
  char           id[40];
  char           number[40];
  char           counterparty_id[40];
  char           issue_date[24];
  char           due_date[24];
  mb_bill_status status;
  char           memo[256];
  char           currency[8];
  char           entry_id[40];
} mb_bill;

typedef struct {
  const char *item_id;      /* nullable */
  const char *description;  /* required */
  int64_t     qty_centi;    /* x100; <=0 => 100 */
  mb_money    unit_price;
  const char *account_id;   /* expense account (or tax account) */
  int         is_tax;
} mb_bill_line_in;

mb_err mb_bill_create(mb_store *s, const char *counterparty_id, const char *number,
                      const char *due_date, const char *memo, char id_out[40]) MB_MUST_CHECK;
mb_err mb_bill_add_line(mb_store *s, const char *bill_id, const mb_bill_line_in *in,
                        char line_id_out[40]) MB_MUST_CHECK;
mb_err mb_bill_total(mb_store *s, const char *bill_id, mb_money *out) MB_MUST_CHECK;
mb_err mb_bill_enter(mb_store *s, const char *bill_id, const char *issue_date) MB_MUST_CHECK;
mb_err mb_bill_get(mb_store *s, const char *id, mb_bill *out) MB_MUST_CHECK;

/* A row for the list view: bill header + counterparty (vendor) name + computed total. */
typedef struct {
  char           id[40];
  char           number[40];
  char           counterparty_id[40];
  char           counterparty_name[128];
  char           issue_date[24];
  char           due_date[24];
  mb_bill_status status;
  mb_money       total;
} mb_bill_row;

/* Newest first. Caller frees *rows_out. */
mb_err mb_bill_list(mb_store *s, mb_bill_row **rows_out, int *n_out) MB_MUST_CHECK;

/* A line of a bill, joined with its account name, for the detail view. */
typedef struct {
  char     id[40];
  char     description[256];
  int64_t  qty_centi;
  mb_money unit_price;
  mb_money line_total;
  char     account_id[40];
  char     account_name[128];
  int      is_tax;
} mb_bill_line;

mb_err mb_bill_lines(mb_store *s, const char *bill_id, mb_bill_line **rows, int *n) MB_MUST_CHECK;

/* Editing (D13 — editable until paid). DRAFT only, except mb_bill_revert_to_draft which reopens an
 * OPEN (unpaid) bill by posting a reversing entry. */
mb_err mb_bill_remove_line(mb_store *s, const char *line_id) MB_MUST_CHECK;
mb_err mb_bill_update(mb_store *s, const char *id, const char *number,
                      const char *due_date, const char *memo) MB_MUST_CHECK;
mb_err mb_bill_revert_to_draft(mb_store *s, const char *id) MB_MUST_CHECK;

#endif /* MB_BILL_H */
