/*
 * Small portable libc shims.
 *
 * The engine uses a handful of POSIX calls (unlink, mkdir, stat, gmtime_r) that
 * either live in different headers or have different signatures on Windows. This
 * maps them onto their Win32 equivalents so the call sites stay byte-identical
 * across platforms. Include this instead of <unistd.h> where those calls appear.
 */
#ifndef MB_SUPPORT_COMPAT_H
#define MB_SUPPORT_COMPAT_H

#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#  include <io.h>      /* _unlink */
#  include <direct.h>  /* _mkdir  */
#  include <stdio.h>   /* FILE, fgetc — for the getline shim */
#  include <stdlib.h>  /* realloc — for the getline shim    */

#  define unlink(path)      _unlink(path)
#  define mkdir(path, mode) _mkdir(path)   /* Win32 _mkdir takes no mode argument */

#  ifndef S_ISDIR
#    define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#  endif

/* POSIX gmtime_r(time_t*, tm*) → Win32 gmtime_s(tm*, time_t*) (note the swapped args). */
static inline struct tm *mb_gmtime_r(const time_t *t, struct tm *out) {
  return gmtime_s(out, t) == 0 ? out : (struct tm *)0;
}
#  define gmtime_r(t, out) mb_gmtime_r((t), (out))

/* MSVC has no ssize_t; SSIZE_T lives in <BaseTsd.h> but we only need the type. */
#  ifndef _SSIZE_T_DEFINED
#    define _SSIZE_T_DEFINED
typedef long long ssize_t;
#  endif

/* Path helpers the native shell (src/app/main.c) uses. PATH_MAX isn't defined on
 * Windows (_MAX_PATH is 260); realpath/getcwd map onto the _f* / _getcwd variants.
 * Note _fullpath's argument order is (dst, src, len) — the reverse of realpath. */
#  ifndef PATH_MAX
#    define PATH_MAX 1024
#  endif
#  define getcwd(buf, n)        _getcwd((buf), (int)(n))
#  define realpath(path, resolved) _fullpath((resolved), (path), PATH_MAX)

/* POSIX getline(3): read a full line (incl. '\n'), growing *lineptr as needed.
 * Returns the byte count read, or -1 at EOF/error. Matches glibc semantics
 * closely enough for the newline-delimited JSON-RPC the MCP server reads. */
static inline ssize_t mb_getline(char **lineptr, size_t *n, FILE *stream) {
  if (!lineptr || !n || !stream) return -1;
  if (*lineptr == (char *)0 || *n == 0) {
    *n = 128;
    char *nb = (char *)realloc(*lineptr, *n);
    if (!nb) return -1;
    *lineptr = nb;
  }
  size_t pos = 0;
  for (;;) {
    int c = fgetc(stream);
    if (c == EOF) { if (pos == 0) return -1; break; }
    if (pos + 1 >= *n) {
      size_t newcap = *n * 2;
      char *nb = (char *)realloc(*lineptr, newcap);
      if (!nb) return -1;
      *lineptr = nb;
      *n = newcap;
    }
    (*lineptr)[pos++] = (char)c;
    if (c == '\n') break;
  }
  (*lineptr)[pos] = '\0';
  return (ssize_t)pos;
}
#  define getline(lineptr, n, stream) mb_getline((lineptr), (n), (stream))

#else
#  include <unistd.h>  /* unlink */
#endif

#endif /* MB_SUPPORT_COMPAT_H */
