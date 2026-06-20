/*
 * Integration test: the Phase 7b share protocol end-to-end over the in-memory loopback
 * transport. A host serve loop runs on its own thread (as it will in the real app); the
 * main thread acts as the guest, calling mb_share_call exactly the way a guest's mbInvoke
 * will. Proves: reads return the host's real data, writes are refused over the wire, and a
 * malformed frame gets an error envelope without killing the session. Zero network, zero Rust.
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../src/support/mb_thread.h"
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

  mb_thread th;
  struct serve_arg sa = { s, &host };
  ASSERT_EQ_INT(mb_thread_create(&th, serve_thread, &sa), 0);

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
  mb_thread_join(th);
  mb_share_loopback_free(st);
  mb_store_close(s);
}

/* The gated serve drops an already-connected guest the moment the owner stops sharing:
 * the next request is refused unanswered and the host closes the link from its own thread.
 * This is what makes the "Stop sharing" button actually revoke a live guest. */
static int gate_cb(void *p) { return atomic_load((_Atomic int *)p); }
struct gated_arg { mb_store *ro; mb_share_transport *t; _Atomic int *open; };
static void *serve_gated_thread(void *p) {
  struct gated_arg *a = p;
  (void)mb_share_serve_gated(a->ro, a->t, gate_cb, a->open);
  a->t->close(a->t->ctx);   /* mirror the app: close from the serving thread → guest sees the drop */
  return NULL;
}

TEST(share, stop_drops_connected_guest) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_starter_chart(s));

  mb_share_transport host, guest;
  void *st = NULL;
  ASSERT_OK(mb_share_loopback_pair(&host, &guest, &st));

  _Atomic int open = 1;
  mb_thread th;
  struct gated_arg ga = { s, &host, &open };
  ASSERT_EQ_INT(mb_thread_create(&th, serve_gated_thread, &ga), 0);

  /* while sharing is on, the guest can read */
  char *r = NULL;
  ASSERT_OK(mb_share_call(&guest, "book.status", "{}", &r));
  ASSERT_TRUE(strstr(r, "error") == NULL);
  free(r);

  /* owner clicks Stop — the very next guest call is dropped and the link closes */
  atomic_store(&open, 0);
  r = NULL;
  ASSERT_ERR(mb_share_call(&guest, "book.status", "{}", &r), MB_ERR_IO);
  ASSERT_TRUE(r == NULL);   /* no response was sent */

  guest.close(guest.ctx);
  mb_thread_join(th);
  mb_share_loopback_free(st);
  mb_store_close(s);
}
