#ifndef MB_ID_H
#define MB_ID_H
/* Money Books — stable IDs. UUIDv4 text, e.g. "550e8400-e29b-41d4-a716-446655440000". */
#include "mb_error.h"

/* Write a random UUIDv4 (36 chars + NUL) into out. buflen must be >= 37. */
mb_err mb_uuid(char *out, size_t buflen) MB_MUST_CHECK;

#endif /* MB_ID_H */
