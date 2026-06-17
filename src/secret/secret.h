#ifndef MB_SECRET_H
#define MB_SECRET_H
/*
 * Money Books — secret store (D22). Thin interface over an OS keystore.
 *
 * Backends:
 *   - macOS Keychain (Security.framework) — default on Apple.
 *   - In-memory — for tests/headless (compile with -DMB_SECRET_MEMORY).
 *   (Windows Credential Manager / Linux libsecret are future backends.)
 *
 * `account` is a stable key like "llm:openai". Values never touch SQLite, logs, or egress.
 */
#include <stddef.h>
#include "../support/mb_error.h"

mb_err mb_secret_set(const char *account, const char *value) MB_MUST_CHECK;
/* Copies the secret into buf; MB_ERR_NOT_FOUND if absent. */
mb_err mb_secret_get(const char *account, char *buf, size_t buflen) MB_MUST_CHECK;
mb_err mb_secret_delete(const char *account);
int    mb_secret_has(const char *account);

#endif /* MB_SECRET_H */
