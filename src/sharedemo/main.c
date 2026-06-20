/*
 * Money Books — iroh share demo (Phase 7b-2). Proves real host↔guest QUIC end-to-end,
 * the same protocol the app will use, before any UI wiring (7b-3).
 *
 *   host:   share-demo host  <book.sqlite>   bind, print the address, serve read-only
 *   guest:  share-demo guest "<address>"     dial, pull reports, print them
 *
 * Built only by `make share-demo` (needs MB_WITH_SHARE + the iroh staticlib). Not part of
 * ENGINE_SRC, so it never collides with the app/mcp/test entry points.
 */
#ifndef MB_WITH_SHARE
#include <stdio.h>
int main(void) { fprintf(stderr, "build with `make share-demo` (needs MB_WITH_SHARE)\n"); return 2; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "store/store.h"
#include "seed/seed.h"
#include "share/share.h"
#include "share/iroh.h"

static int run_host(const char *book) {
  mb_store *s = NULL;
  if (mb_store_open_readonly(book, &s) != MB_OK) {
    /* not there yet (or pre-migration): create + seed a demo book, then reopen read-only */
    fprintf(stderr, "(host) creating/seeding %s\n", book);
    if (mb_store_open(book, &s) != MB_OK) { fprintf(stderr, "open: %s\n", mb_last_error()->message); return 1; }
    (void)mb_seed_starter_chart(s);
    mb_store_close(s);
    if (mb_store_open_readonly(book, &s) != MB_OK) { fprintf(stderr, "reopen ro: %s\n", mb_last_error()->message); return 1; }
  }

  mb_share_endpoint *ep = NULL;
  char addr[1024] = "", key[80] = "";
  if (mb_share_iroh_bind(&ep, addr, sizeof addr, key, sizeof key) != MB_OK) {
    fprintf(stderr, "bind: %s\n", mb_last_error()->message);
    mb_store_close(s);
    return 1;
  }
  printf("HOST READY\nnode key (fingerprint): %s\nADDRESS (give this to the guest):\n%s\n\n", key, addr);
  fflush(stdout);

  for (;;) {
    mb_share_transport t;
    if (mb_share_iroh_accept(ep, &t) != MB_OK) { fprintf(stderr, "accept: %s\n", mb_last_error()->message); continue; }
    fprintf(stderr, "(host) guest connected — serving read-only\n");
    (void)mb_share_serve(s, &t);   /* returns when the guest closes */
    t.close(t.ctx);
    fprintf(stderr, "(host) guest disconnected\n");
  }
}

static int run_guest(const char *addr) {
  setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered so nothing is lost on an early exit */
  setvbuf(stderr, NULL, _IONBF, 0);
  fprintf(stderr, "(guest) connecting...\n");
  mb_share_transport t;
  if (mb_share_iroh_connect(addr, &t) != MB_OK) { fprintf(stderr, "connect: %s\n", mb_last_error()->message); return 1; }
  fprintf(stderr, "(guest) connected\n");

  static const char *const calls[] = { "book.status", "report.pnl", "report.balance_sheet", "account.list", "expense.record" };
  static const char *const args[]  = { "{}",          "{}",         "{}",                   "{}",           "{\"amount\":100}" };
  int rc = 0;
  for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
    fprintf(stderr, "(guest) -> %s\n", calls[i]);
    char *r = NULL;
    if (mb_share_call(&t, calls[i], args[i], &r) != MB_OK) {
      fprintf(stderr, "call %s failed (transport): %s\n", calls[i], mb_last_error()->message);
      rc = 1;
      break;
    }
    printf("%-22s -> %s\n", calls[i], r);
    free(r);
  }
  fprintf(stderr, "(guest) closing\n");
  t.close(t.ctx);
  fprintf(stderr, "(guest) done\n");
  return rc;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s host <book.sqlite> | guest <address>\n", argv[0]); return 2; }
  if (!strcmp(argv[1], "host"))  return run_host(argv[2]);
  if (!strcmp(argv[1], "guest")) return run_guest(argv[2]);
  fprintf(stderr, "unknown mode '%s'\n", argv[1]);
  return 2;
}
#endif /* MB_WITH_SHARE */
