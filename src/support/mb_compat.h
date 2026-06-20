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

#else
#  include <unistd.h>  /* unlink */
#endif

#endif /* MB_SUPPORT_COMPAT_H */
