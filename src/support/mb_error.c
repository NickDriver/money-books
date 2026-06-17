#include "mb_error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local mb_error_ctx g_err;

const char *mb_err_name(mb_err e) {
  switch (e) {
#define MB_NAME(name, msg) case name: return #name;
    MB_ERR_LIST(MB_NAME)
#undef MB_NAME
    default: return "MB_ERR_UNKNOWN";
  }
}

const char *mb_err_default_msg(mb_err e) {
  switch (e) {
#define MB_MSG(name, msg) case name: return msg;
    MB_ERR_LIST(MB_MSG)
#undef MB_MSG
    default: return "unknown error";
  }
}

const mb_error_ctx *mb_last_error(void) { return &g_err; }

void mb_clear_error(void) {
  memset(&g_err, 0, sizeof g_err);
  g_err.code = MB_OK;
}

mb_err mb_set_error(mb_err code, const char *file, int line, const char *func,
                    const char *fmt, ...) {
  g_err.code = code;
  g_err.file = file;
  g_err.line = line;
  g_err.func = func;
  g_err.trace[0] = '\0';
  if (fmt && fmt[0]) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err.message, sizeof g_err.message, fmt, ap);
    va_end(ap);
  } else {
    snprintf(g_err.message, sizeof g_err.message, "%s", mb_err_default_msg(code));
  }
  return code;
}

void mb_error_add_trace(const char *file, int line, const char *func,
                        const char *expr) {
  size_t n = strlen(g_err.trace);
  if (n >= sizeof g_err.trace - 1) return;
  snprintf(g_err.trace + n, sizeof g_err.trace - n, "%s%s:%d %s [%s]",
           n ? " <- " : "", file, line, func, expr);
}

void mb_invariant_fail(const char *expr, const char *file, int line,
                       const char *func, const char *fmt, ...) {
  fprintf(stderr, "\n*** MB_INVARIANT VIOLATED ***\n  at %s:%d in %s()\n  cond: %s\n",
          file, line, func, expr);
  if (fmt && fmt[0]) {
    fputs("  note: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
  }
  fflush(stderr);
  abort();
}

void mb_log(mb_log_level lvl, const char *file, int line, const char *fmt, ...) {
  static const char *tag[] = {"DEBUG", "INFO", "WARN", "ERROR"};
  fprintf(stderr, "[%-5s] %s:%d: ", tag[lvl], file, line);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}
