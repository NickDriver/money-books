/*
 * Integration test: the Phase 7b share protocol end-to-end over the in-memory loopback
 * transport. A host serve loop runs on its own thread (as it will in the real app); the
 * main thread acts as the guest, calling mb_share_call exactly the way a guest's mbInvoke
 * will. Proves: reads return the host's real data, writes are refused over the wire, and a
 * malformed frame gets an error envelope without killing the session. Zero network, zero Rust.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "../src/share/share.h"
#include "../src/share/transport.h"
#include "../src/store/store.h"
#include "../src/seed/seed.h"
#include "../src/support/mb_test.h"
#include "../src/vendor/cjson/cJSON.h"

struct serve_arg { mb_store *ro; mb_share_transport *t; };
static void *serve_thread(void *p) {
  struct serve_arg *a = p;
  (void)mb_share_serve(a->ro, a->t);   /* returns MB_ERR_IO when the guest closes */
  return NULL;
}

/* read the next frame off a transport into a NUL-terminated string (caller frees) */
static char *recv_str(mb_share_transport *t) {
  void *buf = NULL; size_t len = 0;
  if (t->recv(t->ctx, &buf, &len) != MB_OK) return NULL;
  char *s = malloc(len + 1);
  memcpy(s, buf, len);
  s[len] = '\0';
  free(buf);
  return s;
}

TEST(share, protocol_over_loopback) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_starter_chart(s));   /* the host's book */

  mb_share_transport host, guest;
  void *st = NULL;
  ASSERT_OK(mb_share_loopback_pair(&host, &guest, &st));

  pthread_t th;
  struct serve_arg sa = { s, &host };
  ASSERT_EQ_INT(pthread_create(&th, NULL, serve_thread, &sa), 0);

  char *r = NULL;

  /* 1) a read returns the host's real chart, fetched remotely */
  ASSERT_OK(mb_share_call(&guest, "account.list", "{}", &r));
  cJSON *j = cJSON_Parse(r);
  cJSON *accts = cJSON_GetObjectItem(j, "accounts");
  ASSERT_TRUE(cJSON_IsArray(accts));
  ASSERT_TRUE(cJSON_GetArraySize(accts) > 0);
  cJSON_Delete(j);
  free(r);

  /* 2) a report read works over the wire */
  ASSERT_OK(mb_share_call(&guest, "report.pnl", "{}", &r));
  ASSERT_TRUE(strstr(r, "\"net\"") != NULL);
  ASSERT_TRUE(strstr(r, "error") == NULL);
  free(r);

  /* 3) a write is refused — the read-only gate, enforced host-side */
  ASSERT_OK(mb_share_call(&guest, "expense.record", "{\"amount\":100}", &r));
  ASSERT_TRUE(strstr(r, "MB_ERR_PERMISSION") != NULL);
  free(r);

  /* 4) a malformed frame gets an error envelope and the session survives */
  ASSERT_OK(guest.send(guest.ctx, "not json", 8));
  char *m = recv_str(&guest);
  ASSERT_TRUE(m != NULL);
  ASSERT_TRUE(strstr(m, "MB_ERR_PARSE") != NULL);
  free(m);

  /* still alive after the bad frame: one more good call */
  ASSERT_OK(mb_share_call(&guest, "book.status", "{}", &r));
  ASSERT_TRUE(strstr(r, "error") == NULL);
  free(r);

  guest.close(guest.ctx);     /* host serve loop sees EOF and returns */
  pthread_join(th, NULL);
  mb_share_loopback_free(st);
  mb_store_close(s);
}
