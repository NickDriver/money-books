/*
 * Windows implementation of the desktop-shell platform shim (see platform.h).
 * Win32 + the common Save dialog live here so main.c stays platform-free. All paths
 * cross the boundary as UTF-8: we call the wide (…W) APIs and convert at the edges so
 * non-ASCII usernames / book names work regardless of the system ANSI code page.
 */
#include "support/mb_win.h"   /* guarded <windows.h> (WIN32_LEAN_AND_MEAN, NOMB) */
#include <commdlg.h>          /* GetSaveFileNameW — excluded by WIN32_LEAN_AND_MEAN */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

/* UTF-16 → UTF-8 into a caller buffer; returns 0 on success, -1 on failure. */
static int w2u(const wchar_t *w, char *out, size_t out_n) {
  if (!out || out_n == 0) return -1;
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_n, NULL, NULL);
  if (n == 0) { out[0] = '\0'; return -1; }
  return 0;
}

int mb_platform_mcp_binary_path(char *out, size_t out_n) {
  wchar_t exe[MAX_PATH];
  DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) { snprintf(out, out_n, "money-books-mcp.exe"); return -1; }

  /* strip the trailing filename, keeping the separator */
  for (DWORD i = len; i > 0; i--) {
    if (exe[i - 1] == L'\\' || exe[i - 1] == L'/') { exe[i] = L'\0'; break; }
  }
  wchar_t full[MAX_PATH];
  _snwprintf(full, MAX_PATH, L"%smoney-books-mcp.exe", exe);
  if (w2u(full, out, out_n) != 0) { snprintf(out, out_n, "money-books-mcp.exe"); return -1; }
  return 0;
}

int mb_platform_claude_config_path(char *out, size_t out_n) {
  const wchar_t *appdata = _wgetenv(L"APPDATA");   /* %APPDATA% = roaming app data */
  if (!appdata || !appdata[0]) {
    snprintf(out, out_n, "%%APPDATA%%\\Claude\\claude_desktop_config.json");
    return -1;
  }
  wchar_t full[MAX_PATH];
  _snwprintf(full, MAX_PATH, L"%s\\Claude\\claude_desktop_config.json", appdata);
  if (w2u(full, out, out_n) != 0) {
    snprintf(out, out_n, "%%APPDATA%%\\Claude\\claude_desktop_config.json");
    return -1;
  }
  return 0;
}

/* Windows: WebView2 already handles Copy/Paste/Cut/Select-All natively inside text
 * fields, and the window's close button / Alt+F4 quit the app — so unlike macOS
 * (which needs an Edit menu to wire the responder selectors) there is nothing to
 * install here. A Ctrl+Q accelerator would need the window handle, which this hook
 * isn't given; it can be added later via webview_get_window if desired. */
void mb_platform_install_menu(const char *app_name) {
  (void)app_name;
}

int mb_platform_save_file(const char *suggested, const char *content, char *out, size_t out_n) {
  if (out && out_n) out[0] = '\0';

  wchar_t file[MAX_PATH] = {0};
  if (suggested && suggested[0])
    MultiByteToWideChar(CP_UTF8, 0, suggested, -1, file, MAX_PATH);

  OPENFILENAMEW ofn;
  ZeroMemory(&ofn, sizeof ofn);
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner   = GetActiveWindow();
  ofn.lpstrFile   = file;
  ofn.nMaxFile    = MAX_PATH;
  ofn.lpstrFilter = L"All Files\0*.*\0";
  ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;

  if (!GetSaveFileNameW(&ofn)) {
    DWORD e = CommDlgExtendedError();
    if (e == 0) return 0;                       /* user cancelled */
    snprintf(out, out_n, "save dialog error (0x%lx)", (unsigned long)e);
    return -1;
  }

  HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) { snprintf(out, out_n, "could not open file for writing"); return -1; }

  size_t total = content ? strlen(content) : 0, off = 0;
  const char *p = content ? content : "";
  BOOL wrote_ok = TRUE;
  while (off < total) {
    size_t left = total - off;
    DWORD chunk = (DWORD)(left > 0x40000000u ? 0x40000000u : left);
    DWORD wrote = 0;
    if (!WriteFile(h, p + off, chunk, &wrote, NULL) || wrote == 0) { wrote_ok = FALSE; break; }
    off += wrote;
  }
  CloseHandle(h);
  if (!wrote_ok) { snprintf(out, out_n, "could not write file"); return -1; }

  if (w2u(file, out, out_n) != 0) snprintf(out, out_n, "(saved)");
  return 1;
}
