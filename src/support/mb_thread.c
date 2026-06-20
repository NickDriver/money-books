/* Portable threading (see mb_thread.h): pthreads on POSIX, Win32 on Windows. */
#include "mb_thread.h"
#include <stdlib.h>

#ifdef _WIN32

void mb_mutex_init(mb_mutex *m)    { InitializeCriticalSection(m); }
void mb_mutex_lock(mb_mutex *m)    { EnterCriticalSection(m); }
void mb_mutex_unlock(mb_mutex *m)  { LeaveCriticalSection(m); }
void mb_mutex_destroy(mb_mutex *m) { DeleteCriticalSection(m); }

void mb_cond_init(mb_cond *c)               { InitializeConditionVariable(c); }
void mb_cond_wait(mb_cond *c, mb_mutex *m)  { SleepConditionVariableCS(c, m, INFINITE); }
void mb_cond_signal(mb_cond *c)             { WakeConditionVariable(c); }
void mb_cond_broadcast(mb_cond *c)          { WakeAllConditionVariable(c); }
void mb_cond_destroy(mb_cond *c)            { (void)c; }  /* Win32 condition vars need no teardown */

/* Win32 thread entry has a different signature; trampoline through a heap cell so the
 * caller can pass a POSIX-shaped void *(*)(void *). */
struct mb_tramp { mb_thread_fn fn; void *arg; };
static DWORD WINAPI mb_win_trampoline(LPVOID p) {
  struct mb_tramp t = *(struct mb_tramp *)p;
  free(p);
  (void)t.fn(t.arg);
  return 0;
}
int mb_thread_create(mb_thread *t, mb_thread_fn fn, void *arg) {
  struct mb_tramp *tr = malloc(sizeof *tr);
  if (!tr) return -1;
  tr->fn = fn; tr->arg = arg;
  HANDLE h = CreateThread(NULL, 0, mb_win_trampoline, tr, 0, NULL);
  if (!h) { free(tr); return -1; }
  *t = h;
  return 0;
}
int mb_thread_join(mb_thread t)   { WaitForSingleObject(t, INFINITE); CloseHandle(t); return 0; }
int mb_thread_detach(mb_thread t) { CloseHandle(t); return 0; }

#else  /* POSIX */

void mb_mutex_init(mb_mutex *m)    { pthread_mutex_init(m, NULL); }
void mb_mutex_lock(mb_mutex *m)    { pthread_mutex_lock(m); }
void mb_mutex_unlock(mb_mutex *m)  { pthread_mutex_unlock(m); }
void mb_mutex_destroy(mb_mutex *m) { pthread_mutex_destroy(m); }

void mb_cond_init(mb_cond *c)               { pthread_cond_init(c, NULL); }
void mb_cond_wait(mb_cond *c, mb_mutex *m)  { pthread_cond_wait(c, m); }
void mb_cond_signal(mb_cond *c)             { pthread_cond_signal(c); }
void mb_cond_broadcast(mb_cond *c)          { pthread_cond_broadcast(c); }
void mb_cond_destroy(mb_cond *c)            { pthread_cond_destroy(c); }

int mb_thread_create(mb_thread *t, mb_thread_fn fn, void *arg) { return pthread_create(t, NULL, fn, arg); }
int mb_thread_join(mb_thread t)   { return pthread_join(t, NULL); }
int mb_thread_detach(mb_thread t) { return pthread_detach(t); }

#endif
