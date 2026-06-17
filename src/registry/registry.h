#ifndef MB_REGISTRY_H
#define MB_REGISTRY_H
/*
 * Money Books — book registry (multi-company). App-global, lives OUTSIDE any book:
 * a JSON list of known book files at ~/Library/Application Support/MoneyBooks/books.json.
 * One SQLite file = one company/book; this just records where they are + a friendly name +
 * when each was last opened, so a launcher can list/switch them. Portable by design (D23):
 * the only platform bit is the config-dir lookup.
 */
#include "../support/mb_error.h"
#include <stddef.h>

typedef struct {
  char      path[1024];
  char      name[128];
  long long last_opened;   /* unix seconds; 0 if unknown */
} mb_book_ref;

/* Absolute path to the registry JSON file, creating the app config dir if needed. */
mb_err mb_registry_default_path(char *buf, size_t n) MB_MUST_CHECK;

/* The directory new book files are created in by default (the app config dir). */
mb_err mb_registry_books_dir(char *buf, size_t n) MB_MUST_CHECK;

/* All registered books, most-recently-opened first. Allocates *out; caller frees. */
mb_err mb_registry_list(const char *regpath, mb_book_ref **out, int *n) MB_MUST_CHECK;

/* Insert or update the entry for `bookpath`: set `name` if non-NULL, stamp last_opened=`now`. */
mb_err mb_registry_touch(const char *regpath, const char *bookpath,
                         const char *name, long long now) MB_MUST_CHECK;

/* Remove the entry for `bookpath` (does NOT delete the file). Missing entry is not an error. */
mb_err mb_registry_forget(const char *regpath, const char *bookpath) MB_MUST_CHECK;

#endif /* MB_REGISTRY_H */
