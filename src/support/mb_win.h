/*
 * Guarded <windows.h> include — use this instead of including <windows.h> directly.
 *
 * Plain <windows.h> drags in two namespace landmines for this codebase:
 *   - winuser.h defines MB_OK (a MessageBox button flag, 0x0), which collides with
 *     our success code mb_err MB_OK and turns every `MB_OK,` enumerator into `0x0,`.
 *     NOMB excludes the MB_* / MessageBox constants.
 *   - rpcndr.h defines `small` as a typedef for char, breaking any `small` identifier.
 *     WIN32_LEAN_AND_MEAN drops the RPC headers (and winsock1, shell, DDE, crypto).
 * NOMINMAX keeps the min/max macros out for good measure. Define before <windows.h>.
 */
#ifndef MB_SUPPORT_WIN_H
#define MB_SUPPORT_WIN_H

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMB
#    define NOMB
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#endif /* MB_SUPPORT_WIN_H */
