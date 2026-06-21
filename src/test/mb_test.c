#include "../support/mb_test.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mb_test *g_head = NULL;
static mb_test *g_current = NULL;  /* test in flight, for crash reporting */

void mb_test_register(mb_test *t) {
  t->next = g_head;
  g_head = t;
}

void mb_test_fail(mb_test *t, const char *file, int line, const char *fmt, ...) {
  t->failures++;
  fprintf(stderr, "  \xE2\x9C\x97 %s.%s\n    %s:%d: ", t->suite, t->name, file, line);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static int by_location(const void *A, const void *B) {
  const mb_test *a = *(const mb_test *const *)A;
  const mb_test *b = *(const mb_test *const *)B;
  int c = strcmp(a->file, b->file);
  if (c) return c;
  if (a->line != b->line) return a->line - b->line;
  return strcmp(a->name, b->name);
}

static void on_signal(int sig) {
  if (g_current)
    fprintf(stderr, "\n*** crash (signal %d) during %s.%s (%s:%d) ***\n", sig,
            g_current->suite, g_current->name, g_current->file, g_current->line);
  signal(sig, SIG_DFL);
  raise(sig);
}

int mb_test_main(int argc, char **argv) {
  const char *filter = NULL;
  int json = 0, list = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
    else if (!strcmp(argv[i], "--json")) json = 1;
    else if (!strcmp(argv[i], "--list")) list = 1;
  }

  int n = 0;
  for (mb_test *t = g_head; t; t = t->next) n++;
  mb_test **arr = calloc((size_t)(n > 0 ? n : 1), sizeof *arr);
  int idx = 0;
  for (mb_test *t = g_head; t; t = t->next) arr[idx++] = t;
  qsort(arr, (size_t)n, sizeof *arr, by_location);

  signal(SIGSEGV, on_signal);
  signal(SIGABRT, on_signal);
#ifdef SIGBUS
  signal(SIGBUS, on_signal);   /* not defined in the Windows CRT */
#endif

  int run = 0, passed = 0, failed = 0;
  for (int i = 0; i < n; i++) {
    mb_test *t = arr[i];
    if (filter && !strstr(t->name, filter) && !strstr(t->suite, filter)) continue;
    if (list) {
      printf("%s.%s\t%s:%d\n", t->suite, t->name, t->file, t->line);
      continue;
    }
    g_current = t;
    t->failures = 0;
    mb_clear_error();
    t->fn(t);
    g_current = NULL;
    run++;
    if (t->failures == 0) {
      passed++;
      if (!json) printf("  \xE2\x9C\x93 %s.%s\n", t->suite, t->name);
    } else {
      failed++;
    }
    if (json)
      printf("{\"suite\":\"%s\",\"name\":\"%s\",\"file\":\"%s\",\"line\":%d,"
             "\"ok\":%s,\"failures\":%d}\n",
             t->suite, t->name, t->file, t->line, t->failures ? "false" : "true",
             t->failures);
  }

  if (list) {
    free(arr);
    return 0;
  }

  printf("\n%d run, %d passed, %d failed\n", run, passed, failed);
  if (failed) {
    printf("\nFailing tests (re-run individually):\n");
    for (int i = 0; i < n; i++)
      if (arr[i]->failures)
        printf("  ./build/test_runner --filter %s\n", arr[i]->name);
  }
  free(arr);
  return failed ? 1 : 0;
}
