#ifndef MB_COUNTERPARTY_H
#define MB_COUNTERPARTY_H
/*
 * Money Books — counterparties (clients/vendors), Phase 2, SPEC §4.2.
 * Name is sensitive (D11): pseudonymized before cloud egress (handled at the egress layer).
 */
#include "../store/store.h"

typedef enum { MB_CP_CUSTOMER, MB_CP_VENDOR, MB_CP_BOTH } mb_counterparty_kind;

typedef struct {
  char                 id[40];
  char                 name[128];
  mb_counterparty_kind kind;
  char                 email[128];
  char                 phone[48];
  char                 address[256];
  char                 note[256];
  int                  is_active;
} mb_counterparty;

typedef struct {
  const char          *name;     /* required */
  mb_counterparty_kind kind;
  const char          *email;    /* nullable */
  const char          *phone;    /* nullable */
  const char          *address;  /* nullable */
  const char          *note;     /* nullable */
} mb_counterparty_new;

const char *mb_counterparty_kind_str(mb_counterparty_kind k);

mb_err mb_counterparty_create(mb_store *s, const mb_counterparty_new *in, char id_out[40]) MB_MUST_CHECK;
mb_err mb_counterparty_get(mb_store *s, const char *id, mb_counterparty *out) MB_MUST_CHECK;
mb_err mb_counterparty_set_active(mb_store *s, const char *id, int active) MB_MUST_CHECK;
mb_err mb_counterparty_count(mb_store *s, int *out) MB_MUST_CHECK;

/* List counterparties (ordered by name). Allocates *out (caller frees with free()). */
mb_err mb_counterparty_list(mb_store *s, int active_only, mb_counterparty **out, int *n) MB_MUST_CHECK;

#endif /* MB_COUNTERPARTY_H */
