/*
 * Native macOS Save dialog (NSSavePanel) for the WKWebView shell.
 *
 * WKWebView ignores the HTML `<a download>` trick, so exports from the UI never
 * reach disk in the shipped app. The JS bridge calls window.mbSaveFile(name, text);
 * main.c binds that to mb_save_panel, which shows a real Save sheet and writes the file.
 *
 * Pure-C interface (no Objective-C in the signature) so main.c can include it directly.
 * Implemented in savepanel.m. Must be called on the main/UI thread (runModal is modal).
 */
#ifndef MB_APP_SAVEPANEL_H
#define MB_APP_SAVEPANEL_H

#include <stddef.h>

/* Show a Save dialog seeded with `suggested` (e.g. "profit_and_loss.csv") and, if the
 * user confirms, write `content` (NUL-terminated UTF-8) to the chosen file.
 * Returns:
 *    1  saved — `out` holds the absolute path written
 *    0  user cancelled — `out` is empty
 *   -1  error — `out` holds a human-readable message
 * `out`/`out_n` is always NUL-terminated on return. */
int mb_save_panel(const char *suggested, const char *content, char *out, size_t out_n);

#endif /* MB_APP_SAVEPANEL_H */
