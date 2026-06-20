/* In-memory loopback transport (see transport.h): two thread-safe blocking channels wired
 * head-to-tail, so a host serve loop and a guest caller can run on separate threads. */
#include "transport.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* one direction: a FIFO of owned byte buffers, with blocking pop and a closed flag */
typedef struct node { void *buf; size_t len; struct node *next; } node;
typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t  cond;
  node *head, *tail;
  int closed;
} chan;

typedef struct { chan *send_ch, *recv_ch; } endpoint;
typedef struct { chan g2h, h2g; endpoint he, ge; } loop_state;

static void chan_init(chan *c) {
  pthread_mutex_init(&c->mu, NULL);
  pthread_cond_init(&c->cond, NULL);
  c->head = c->tail = NULL;
  c->closed = 0;
}
static void chan_destroy(chan *c) {
  for (node *n = c->head; n; ) { node *x = n->next; free(n->buf); free(n); n = x; }
  pthread_mutex_destroy(&c->mu);
  pthread_cond_destroy(&c->cond);
}
static mb_err chan_push(chan *c, const void *buf, size_t len) {
  node *n = calloc(1, sizeof *n);
  if (!n) return MB_FAIL(MB_ERR_INTERNAL, "oom");
  n->buf = malloc(len ? len : 1);
  if (!n->buf) { free(n); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  memcpy(n->buf, buf, len);
  n->len = len;
  pthread_mutex_lock(&c->mu);
  if (c->closed) { pthread_mutex_unlock(&c->mu); free(n->buf); free(n); return MB_FAIL(MB_ERR_IO, "channel closed"); }
  if (c->tail) c->tail->next = n; else c->head = n;
  c->tail = n;
  pthread_cond_signal(&c->cond);
  pthread_mutex_unlock(&c->mu);
  return MB_OK;
}
static mb_err chan_pop(chan *c, void **buf, size_t *len) {
  pthread_mutex_lock(&c->mu);
  while (!c->head && !c->closed) pthread_cond_wait(&c->cond, &c->mu);
  if (!c->head) { pthread_mutex_unlock(&c->mu); return MB_FAIL(MB_ERR_IO, "channel closed"); }
  node *n = c->head;
  c->head = n->next;
  if (!c->head) c->tail = NULL;
  pthread_mutex_unlock(&c->mu);
  *buf = n->buf; *len = n->len;
  free(n);
  return MB_OK;
}
static void chan_close(chan *c) {
  pthread_mutex_lock(&c->mu);
  c->closed = 1;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mu);
}

static mb_err ep_send(void *ctx, const void *buf, size_t len) { return chan_push(((endpoint *)ctx)->send_ch, buf, len); }
static mb_err ep_recv(void *ctx, void **buf, size_t *len)     { return chan_pop (((endpoint *)ctx)->recv_ch, buf, len); }
static void   ep_close(void *ctx) { endpoint *e = ctx; chan_close(e->send_ch); chan_close(e->recv_ch); }

mb_err mb_share_loopback_pair(mb_share_transport *host, mb_share_transport *guest, void **state) {
  loop_state *st = calloc(1, sizeof *st);
  if (!st) return MB_FAIL(MB_ERR_INTERNAL, "oom");
  chan_init(&st->g2h);
  chan_init(&st->h2g);
  st->he.send_ch = &st->h2g; st->he.recv_ch = &st->g2h;   /* host sends to guest, reads from guest */
  st->ge.send_ch = &st->g2h; st->ge.recv_ch = &st->h2g;   /* guest sends to host, reads from host  */
  host->send = ep_send;  host->recv = ep_recv;  host->close = ep_close;  host->ctx = &st->he;
  guest->send = ep_send; guest->recv = ep_recv; guest->close = ep_close; guest->ctx = &st->ge;
  *state = st;
  return MB_OK;
}
void mb_share_loopback_free(void *state) {
  loop_state *st = state;
  if (!st) return;
  chan_destroy(&st->g2h);
  chan_destroy(&st->h2g);
  free(st);
}
