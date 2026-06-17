#ifndef MB_ACCOUNT_H
#define MB_ACCOUNT_H
/*
 * Money Books — chart of accounts (Phase 1, SPEC §4.1).
 * One model; friendly "Account"/"Category" labels are the `role` lens (D6).
 */
#include "../store/store.h"

typedef enum {
  MB_ACCT_ASSET, MB_ACCT_LIABILITY, MB_ACCT_EQUITY, MB_ACCT_INCOME, MB_ACCT_EXPENSE
} mb_account_type;

typedef enum { MB_ROLE_SYSTEM, MB_ROLE_ACCOUNT, MB_ROLE_CATEGORY } mb_account_role;

typedef struct {
  char            id[40];
  char            code[32];        /* "" if none */
  char            name[128];
  mb_account_type type;
  mb_account_role role;
  char            parent_id[40];   /* "" if none */
  char            currency[8];
  int             is_active;
} mb_account;

typedef struct {
  const char     *code;       /* nullable */
  const char     *name;       /* required */
  mb_account_type type;
  mb_account_role role;
  const char     *parent_id;  /* nullable */
  const char     *currency;   /* nullable -> book currency */
} mb_account_new;

const char *mb_account_type_str(mb_account_type t);
const char *mb_account_role_str(mb_account_role r);

/* optional filters for listing (0/NULL fields = no filter) */
typedef struct {
  int             has_type;
  mb_account_type type;
  int             has_role;
  mb_account_role role;
  int             active_only;
} mb_account_filter;

mb_err mb_account_create(mb_store *s, const mb_account_new *in, char id_out[40]) MB_MUST_CHECK;
mb_err mb_account_get(mb_store *s, const char *id, mb_account *out) MB_MUST_CHECK;
mb_err mb_account_find_by_code(mb_store *s, const char *code, mb_account *out) MB_MUST_CHECK;
mb_err mb_account_set_active(mb_store *s, const char *id, int active) MB_MUST_CHECK;
/* Rename / recode an account or category. Type and role are intentionally immutable (changing the
 * type of an account that already has postings would corrupt reports). `code` may be "" / NULL. */
mb_err mb_account_update(mb_store *s, const char *id, const char *code, const char *name) MB_MUST_CHECK;
mb_err mb_account_count(mb_store *s, int *out) MB_MUST_CHECK;

/* List accounts (ordered by code, name). Allocates *out (caller frees with free()). */
mb_err mb_account_list(mb_store *s, const mb_account_filter *f,
                       mb_account **out, int *count) MB_MUST_CHECK;

#endif /* MB_ACCOUNT_H */
