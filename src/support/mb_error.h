#ifndef MB_ERROR_H
#define MB_ERROR_H
/*
 * Money Books — error handling & diagnostics (Phase 0).
 *
 * AI-first design goal: every failure is greppable, located, and explained,
 * so an LLM (or human) reading the output can diagnose without a debugger.
 *
 *   - Expected/runtime failures  -> return an `mb_err` code (+ rich last-error).
 *   - Programmer errors           -> MB_DEBUG_ASSERT (compiled out in release).
 *   - Data-integrity violations   -> MB_INVARIANT (ALWAYS on; crash > corrupt books).
 *   - Ignored error codes         -> compile error (MB_MUST_CHECK).
 */
#include <stdbool.h>
#include <stddef.h>

/* ---- compiler attributes ---- */
#if defined(__GNUC__) || defined(__clang__)
#  define MB_MUST_CHECK   __attribute__((warn_unused_result))
#  define MB_UNUSED       __attribute__((unused))
#  define MB_PRINTF(f, a) __attribute__((format(printf, f, a)))
#  define MB_NORETURN     __attribute__((noreturn))
#  define MB_LIKELY(x)    __builtin_expect(!!(x), 1)
#  define MB_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#else
#  define MB_MUST_CHECK
#  define MB_UNUSED
#  define MB_PRINTF(f, a)
#  define MB_NORETURN
#  define MB_LIKELY(x)   (x)
#  define MB_UNLIKELY(x) (x)
#endif

/* ---- error codes: single source of truth via X-macro ----
 * Add a code here and mb_err_name()/mb_err_default_msg() update automatically. */
#define MB_ERR_LIST(X) \
  X(MB_OK,              "ok") \
  X(MB_ERR_INVALID_ARG,"invalid argument") \
  X(MB_ERR_NOT_FOUND,  "not found") \
  X(MB_ERR_EXISTS,     "already exists") \
  X(MB_ERR_UNBALANCED, "journal entry does not balance") \
  X(MB_ERR_OVERFLOW,   "monetary overflow") \
  X(MB_ERR_PARSE,      "parse error") \
  X(MB_ERR_IO,         "I/O error") \
  X(MB_ERR_DB,         "database error") \
  X(MB_ERR_CONFLICT,   "conflict") \
  X(MB_ERR_PERMISSION, "permission denied") \
  X(MB_ERR_UNSUPPORTED,"unsupported operation") \
  X(MB_ERR_INTERNAL,   "internal error")

typedef enum {
#define MB_ENUM(name, msg) name,
  MB_ERR_LIST(MB_ENUM)
#undef MB_ENUM
  MB_ERR__COUNT
} mb_err;

const char *mb_err_name(mb_err e);        /* e.g. "MB_ERR_NOT_FOUND" */
const char *mb_err_default_msg(mb_err e); /* e.g. "not found"        */

/* ---- last-error context (thread-local) ---- */
typedef struct {
  mb_err      code;
  char        message[256];
  const char *file;
  int         line;
  const char *func;
  char        trace[512];   /* breadcrumb chain accumulated by MB_TRY */
} mb_error_ctx;

const mb_error_ctx *mb_last_error(void);
void               mb_clear_error(void);

/* set + return the error; prefer the MB_FAIL macro below */
mb_err mb_set_error(mb_err code, const char *file, int line,
                    const char *func, const char *fmt, ...) MB_PRINTF(5, 6);
void   mb_error_add_trace(const char *file, int line, const char *func,
                          const char *expr);

/* MB_FAIL(code)  or  MB_FAIL(code, "ctx %d", x) */
#define MB_FAIL(code, ...) \
  mb_set_error((code), __FILE__, __LINE__, __func__, "" __VA_ARGS__)

/* propagate like Rust's `?`, appending a breadcrumb to the trace */
#define MB_TRY(expr) \
  do { mb_err _mb_e = (expr); \
       if (MB_UNLIKELY(_mb_e != MB_OK)) { \
         mb_error_add_trace(__FILE__, __LINE__, __func__, #expr); \
         return _mb_e; } } while (0)

/* ---- invariants vs debug asserts ---- */
MB_NORETURN void mb_invariant_fail(const char *expr, const char *file, int line,
                                   const char *func, const char *fmt, ...)
    MB_PRINTF(5, 6);

/* ALWAYS on (even with NDEBUG): integrity guards. Aborts with a full diagnostic. */
#define MB_INVARIANT(cond, ...) \
  do { if (MB_UNLIKELY(!(cond))) \
         mb_invariant_fail(#cond, __FILE__, __LINE__, __func__, "" __VA_ARGS__); \
  } while (0)

/* Compiled out under NDEBUG: ordinary bug-catching. */
#ifdef NDEBUG
#  define MB_DEBUG_ASSERT(cond, ...) ((void)0)
#else
#  define MB_DEBUG_ASSERT(cond, ...) MB_INVARIANT((cond), "" __VA_ARGS__)
#endif

/* ---- minimal leveled logging (stderr, located) ---- */
typedef enum { MB_LOG_DEBUG, MB_LOG_INFO, MB_LOG_WARN, MB_LOG_ERROR } mb_log_level;
void mb_log(mb_log_level lvl, const char *file, int line, const char *fmt, ...)
    MB_PRINTF(4, 5);
#define MB_LOGD(...) mb_log(MB_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define MB_LOGI(...) mb_log(MB_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define MB_LOGW(...) mb_log(MB_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define MB_LOGE(...) mb_log(MB_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif /* MB_ERROR_H */
