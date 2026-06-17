#ifndef MB_PAYMENT_H
#define MB_PAYMENT_H
/*
 * Money Books — payments (Phase 2, SPEC §4.3).
 *   Invoice receipt: Dr Cash/Bank, Cr Accounts Receivable.
 *   Bill payment:    Dr Accounts Payable, Cr Cash/Bank.
 * Updates the target's status to PARTIAL/PAID based on cumulative payments.
 */
#include "../store/store.h"
#include "../money/money.h"

typedef enum { MB_PAY_INVOICE, MB_PAY_BILL } mb_pay_target;

mb_err mb_payment_record(mb_store *s, const char *date, mb_money amount,
                         const char *cash_account_id, mb_pay_target target,
                         const char *target_id, char id_out[40]) MB_MUST_CHECK;

/* sum of cash payments recorded against a target (not the allocated portion) */
mb_err mb_payment_total_for(mb_store *s, mb_pay_target target, const char *target_id,
                            mb_money *out) MB_MUST_CHECK;

/* ---- D26: per-counterparty balance-forward + manual credit application ---- */

/* A counterparty's running AR (invoices) or AP (bills) balance, debit-normal signed cents:
 * positive = they owe us / we owe them on the control account; negative = they are in credit.
 * Computed as Σ of the control-account postings tagged with this counterparty. */
mb_err mb_counterparty_balance(mb_store *s, const char *counterparty_id, mb_pay_target target,
                               mb_money *out) MB_MUST_CHECK;

/* Unapplied credit available to draw against open documents for this counterparty/side:
 * Σ(its cash payments) − Σ(its allocations). Always >= 0. */
mb_err mb_credit_available(mb_store *s, const char *counterparty_id, mb_pay_target target,
                           mb_money *out) MB_MUST_CHECK;

/* Manually apply existing available credit to an OPEN/PARTIAL document (no cash, no journal
 * entry — the AR/AP already reflects the prepayment). Fails if the document is not payable,
 * amount <= 0, amount exceeds the document's remaining balance, or exceeds available credit. */
mb_err mb_credit_apply(mb_store *s, const char *date, const char *counterparty_id,
                       mb_pay_target target, const char *target_id, mb_money amount,
                       char id_out[40]) MB_MUST_CHECK;

#endif /* MB_PAYMENT_H */
