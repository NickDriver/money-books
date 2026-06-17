#include "book.h"

#include <stdio.h>
#include <string.h>
#include "../seed/seed.h"

mb_err mb_book_company_name(mb_store *s, char *buf, size_t n) {
  if (mb_store_meta_get(s, "company_name", buf, n) != MB_OK) { if (n) buf[0] = '\0'; }
  return MB_OK;
}

mb_err mb_book_set_company_name(mb_store *s, const char *name) {
  return mb_store_meta_set(s, "company_name", name ? name : "");
}

mb_err mb_book_create(const char *path, const char *name, const char *template_) {
  if (!path || !path[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "path required");
  if (!name || !name[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "company name required");
  /* refuse to clobber an existing file */
  FILE *probe = fopen(path, "rb");
  if (probe) { fclose(probe); return MB_FAIL(MB_ERR_EXISTS, "a book already exists at '%s'", path); }

  mb_store *s = NULL;
  MB_TRY(mb_store_open(path, &s));   /* creates the file + runs migrations */
  mb_err e;
  if (template_ && !strcmp(template_, "freelancer")) e = mb_seed_starter_chart(s);
  else                                               e = mb_seed_system_accounts(s);
  if (e == MB_OK) e = mb_book_set_company_name(s, name);
  mb_store_close(s);
  return e;
}

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../account/account.h"
#include <unistd.h>

TEST(book, create_seeds_and_names) {
  const char *path = "/tmp/mb_book_create_test.sqlite";
  unlink(path);

  ASSERT_OK(mb_book_create(path, "Acme LLC", "freelancer"));

  mb_store *s = NULL; ASSERT_OK(mb_store_open(path, &s));
  int n = 0; ASSERT_OK(mb_account_count(s, &n));
  ASSERT(n > 4);                                  /* full starter chart, not just system accounts */
  char nm[128]; ASSERT_OK(mb_book_company_name(s, nm, sizeof nm));
  ASSERT_STR_EQ(nm, "Acme LLC");
  mb_store_close(s);

  /* creating over an existing file is refused */
  ASSERT_ERR(mb_book_create(path, "Dup", "empty"), MB_ERR_EXISTS);
  unlink(path);
}

TEST(book, create_empty_template) {
  const char *path = "/tmp/mb_book_empty_test.sqlite";
  unlink(path);
  ASSERT_OK(mb_book_create(path, "Bare Co", "empty"));
  mb_store *s = NULL; ASSERT_OK(mb_store_open(path, &s));
  int n = 0; ASSERT_OK(mb_account_count(s, &n));
  ASSERT_EQ_INT(n, 4);                            /* system accounts only */
  mb_store_close(s);
  unlink(path);
}
#endif
