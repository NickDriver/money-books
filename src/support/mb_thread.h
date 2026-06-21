/*
 * Minimal portable threading — pthreads on POSIX, Win32 on Windows.
 *
 * macOS clang ships no C11 <threads.h>, so this is the portable common denominator:
 * exactly what the in-memory loopback transport and the share tests/app need — a
 * mutex, a condition variable, and joinable/detachable threads with a uniform
 * void *(*)(void *) entry point. Bodies live in mb_thread.c.
 */
#ifndef MB_SUPPORT_THREAD_H
#define MB_SUPPORT_THREAD_H

#ifdef _WIN32
#  include "mb_win.h"   /* guarded <windows.h> (NOMB avoids the MB_OK collision) */
typedef CRITICAL_SECTION   mb_mutex;
typedef CONDITION_VARIABLE mb_cond;
typedef HANDLE             mb_thread;
#else
#  include <pthread.h>
typedef pthread_mutex_t mb_mutex;
typedef pthread_cond_t  mb_cond;
typedef pthread_t       mb_thread;
#endif

typedef void *(*mb_thread_fn)(void *);

void mb_mutex_init(mb_mutex *m);
void mb_mutex_lock(mb_mutex *m);
void mb_mutex_unlock(mb_mutex *m);
void mb_mutex_destroy(mb_mutex *m);

void mb_cond_init(mb_cond *c);
void mb_cond_wait(mb_cond *c, mb_mutex *m);   /* releases m while waiting, reacquires on wake */
void mb_cond_signal(mb_cond *c);
void mb_cond_broadcast(mb_cond *c);
void mb_cond_destroy(mb_cond *c);

int mb_thread_create(mb_thread *t, mb_thread_fn fn, void *arg);  /* 0 on success */
int mb_thread_join(mb_thread t);
int mb_thread_detach(mb_thread t);

#endif /* MB_SUPPORT_THREAD_H */
