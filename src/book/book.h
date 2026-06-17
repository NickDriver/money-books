#ifndef MB_BOOK_H
#define MB_BOOK_H
/*
 * Money Books — book (company) lifecycle helpers for multi-company support.
 * A book is one SQLite file = one company. Creating a book = open (which migrates), seed the
 * chosen chart, and record its company name. Because a seeded book has accounts, book.status
 * already reports it as onboarded (no wizard).
 */
#include "../store/store.h"

/* Create a brand-new book at `path` (must not already exist), seed it from `template_`
 * ("freelancer" → starter chart, anything else → system accounts only), and set its company name. */
mb_err mb_book_create(const char *path, const char *name, const char *template_) MB_MUST_CHECK;

/* Read/write the company name stored in the book (book_meta). Missing name → "" (not an error). */
mb_err mb_book_company_name(mb_store *s, char *buf, size_t n) MB_MUST_CHECK;
mb_err mb_book_set_company_name(mb_store *s, const char *name) MB_MUST_CHECK;

#endif /* MB_BOOK_H */
