/*
 * Desktop-shell platform shim.
 *
 * The accounting engine is portable C and needs none of this — only the native
 * window shell (main.c) does. Exactly one implementation compiles per OS
 * (platform_mac.m today; platform_win.c / platform_posix.c as the port lands),
 * so main.c carries no per-OS #ifdefs. All functions are main/UI-thread only (the
 * dialogs are modal) and always NUL-terminate out within out_n.
 *
 * Pure-C signatures (no Objective-C/COM types leak out) so main.c includes this
 * directly regardless of the implementation language.
 */
#ifndef MB_APP_PLATFORM_H
#define MB_APP_PLATFORM_H

#include <stddef.h>

/* Absolute path to the sibling money-books-mcp[.exe] binary — it ships in the same
 * directory as this executable (true in the dev build dir and in a packaged bundle).
 * Returns 0 on success; -1 if the executable path couldn't be resolved, in which
 * case `out` holds a bare relative fallback name. */
int mb_platform_mcp_binary_path(char *out, size_t out_n);

/* Absolute path to the user's Claude Desktop config file (claude_desktop_config.json
 * under the platform's per-user app-data location). Returns 0 on success; -1 if that
 * location can't be determined, in which case `out` holds a best-effort fallback. */
int mb_platform_claude_config_path(char *out, size_t out_n);

/* Native "Save As" dialog seeded with `suggested` (e.g. "profit_and_loss.csv"); on
 * confirm, writes `content` (NUL-terminated UTF-8) to the chosen file. Returns:
 *    1  saved      — `out` holds the absolute path written
 *    0  cancelled  — `out` is empty
 *   -1  error      — `out` holds a human-readable message */
int mb_platform_save_file(const char *suggested, const char *content, char *out, size_t out_n);

/* Install the native edit/quit keyboard shortcuts for the app window:
 *   Copy   ⌘C / Ctrl+C      Paste  ⌘V / Ctrl+V      Quit/Close  ⌘Q / Ctrl+Q
 * plus the rest of the standard edit set (Cut/Select All/Undo) on platforms that
 * surface a menu. macOS: a WKWebView has no working Copy/Paste until an Edit menu
 * wires the `copy:`/`paste:` responder selectors, and Quit needs a `terminate:`
 * item — this builds that main menu. Windows (WebView2): Copy/Paste are native in
 * text fields, so the impl only needs to add a Ctrl+Q accelerator. Call once, on
 * the UI thread, after the webview is created and before the run loop starts.
 * `app_name` labels the macOS app menu (falls back to "Money Books" if NULL). */
void mb_platform_install_menu(const char *app_name);

#endif /* MB_APP_PLATFORM_H */
