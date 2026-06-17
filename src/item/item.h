#ifndef MB_ITEM_H
#define MB_ITEM_H
/*
 * Money Books — service/expense item dictionaries (Phase 2, D14, SPEC §4.2).
 * Reusable templates: a name + default price + linked income/expense account.
 */
#include "../store/store.h"
#include "../money/money.h"

typedef enum { MB_ITEM_SERVICE, MB_ITEM_EXPENSE } mb_item_kind;

typedef struct {
  char         id[40];
  mb_item_kind kind;
  char         name[128];
  mb_money     default_unit_price;   /* cents */
  char         default_account_id[40]; /* "" if none */
  char         unit_label[32];       /* "" if none */
  int          is_active;
} mb_item;

typedef struct {
  mb_item_kind kind;
  const char  *name;                 /* required */
  mb_money     default_unit_price;
  const char  *default_account_id;   /* nullable; must exist if given */
  const char  *unit_label;           /* nullable */
} mb_item_new;

const char *mb_item_kind_str(mb_item_kind k);

mb_err mb_item_create(mb_store *s, const mb_item_new *in, char id_out[40]) MB_MUST_CHECK;
mb_err mb_item_get(mb_store *s, const char *id, mb_item *out) MB_MUST_CHECK;
mb_err mb_item_set_active(mb_store *s, const char *id, int active) MB_MUST_CHECK;
mb_err mb_item_count(mb_store *s, int *out) MB_MUST_CHECK;
/* List items (ordered by name). active_only filters out deactivated items. Allocates *out. */
mb_err mb_item_list(mb_store *s, int active_only, mb_item **out, int *n) MB_MUST_CHECK;

#endif /* MB_ITEM_H */
