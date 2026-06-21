/*
 * iroh QUIC implementation of mb_share_transport (see iroh.h / transport.h).
 *
 * The ONLY file that includes irohnet.h and links the Rust staticlib. Compiled into a real
 * transport only with -DMB_WITH_SHARE (the share/app build); in pure-C builds the body is
 * empty (the typedef below keeps it a legal, non-empty translation unit under -Wpedantic).
 *
 * Protocol mapping: one persistent bidirectional stream per guest carries length-prefixed
 * frames (u32 big-endian length + JSON bytes) — exactly the mb_share_transport contract, so
 * share.c's serve/call loops run over iroh unchanged. The guest opens the stream and writes
 * first (mb_share_call); the host accepts and reads first (mb_share_serve).
 */
typedef int mb_share_iroh_tu_guard;  /* non-empty TU when sharing is compiled out */

#ifdef MB_WITH_SHARE

#include "iroh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "irohnet.h"

#define MB_SHARE_ALPN      "money-books/share/1"
#define MB_SHARE_MAX_FRAME (64u * 1024u * 1024u)   /* reject absurd lengths from a bad peer */

/* per-connection transport state behind mb_share_transport.ctx */
typedef struct {
  Endpoint_t   *ep;       /* freed on close only when owns_ep (guest owns its endpoint) */
  int           owns_ep;
  Connection_t *conn;
  SendStream_t *send;
  RecvStream_t *recv;
} iroh_ctx;

struct mb_share_endpoint { Endpoint_t *ep; };

static slice_ref_uint8_t share_alpn(void) {
  static const char alpn[] = MB_SHARE_ALPN;
  slice_ref_uint8_t s = { (const uint8_t *)alpn, sizeof alpn - 1 };
  return s;
}

/* ---- length-prefixed framing over the bidi stream ---- */
static mb_err io_send(iroh_ctx *c, const void *buf, size_t len) {
  uint8_t hdr[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
  slice_ref_uint8_t h = { hdr, 4 };
  if (send_stream_write(&c->send, h) != ENDPOINT_RESULT_OK) return MB_FAIL(MB_ERR_IO, "iroh: write header");
  if (len) {
    slice_ref_uint8_t p = { (const uint8_t *)buf, len };
    if (send_stream_write(&c->send, p) != ENDPOINT_RESULT_OK) return MB_FAIL(MB_ERR_IO, "iroh: write body");
  }
  return MB_OK;
}

static mb_err read_exact(iroh_ctx *c, uint8_t *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    slice_mut_uint8_t s = { buf + got, n - got };
    int64_t r = recv_stream_read(&c->recv, s);
    if (r <= 0) return MB_FAIL(MB_ERR_IO, "iroh: stream closed");   /* 0 = EOF, -1 = error */
    got += (size_t)r;
  }
  return MB_OK;
}

static mb_err io_recv(iroh_ctx *c, void **buf, size_t *len) {
  uint8_t hdr[4];
  MB_TRY(read_exact(c, hdr, 4));
  uint32_t n = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
  if (n > MB_SHARE_MAX_FRAME) return MB_FAIL(MB_ERR_IO, "iroh: frame too large (%u)", n);
  uint8_t *b = malloc(n ? n : 1);
  if (!b) return MB_FAIL(MB_ERR_INTERNAL, "oom");
  mb_err e = read_exact(c, b, n);
  if (e != MB_OK) { free(b); return e; }
  *buf = b; *len = n;
  return MB_OK;
}

/* ---- mb_share_transport vtable ---- */
static mb_err t_send(void *ctx, const void *buf, size_t len) { return io_send(ctx, buf, len); }
static mb_err t_recv(void *ctx, void **buf, size_t *len)     { return io_recv(ctx, buf, len); }
static void   t_close(void *ctx) {
  iroh_ctx *c = ctx;
  if (!c) return;
  /* send_stream_finish consumes/half-closes the stream (signals EOF to the peer); the iroh
   * client example never calls send_stream_free after it — doing so segfaults. */
  if (c->send) send_stream_finish(c->send);
  if (c->recv) recv_stream_free(c->recv);
  if (c->conn) connection_free(c->conn);
  if (c->owns_ep && c->ep) endpoint_close(c->ep);
  free(c);
}
static void fill_transport(mb_share_transport *t, iroh_ctx *c) {
  t->send = t_send; t->recv = t_recv; t->close = t_close; t->ctx = c;
}

/* ---- host ---- */
mb_err mb_share_iroh_bind(mb_share_endpoint **out, char *out_addr, size_t addr_n,
                          char *out_key, size_t key_n) {
  EndpointConfig_t cfg = endpoint_config_default();
  endpoint_config_add_alpn(&cfg, share_alpn());
  Endpoint_t *ep = endpoint_default();
  if (endpoint_bind(&cfg, NULL, NULL, &ep) != ENDPOINT_RESULT_OK)
    return MB_FAIL(MB_ERR_IO, "iroh: endpoint bind failed");

  /* Wait until the endpoint is online — iroh defines that as having a home relay
   * AND at least one direct address. Without this we read the address too early and
   * advertise only raw LAN IPs (relay_url=None); a guest then has no path unless a
   * direct UDP connection to us succeeds (blocked by the host's firewall/NAT, or even
   * Wi-Fi client isolation on the same LAN). Waiting puts a relay URL in the address
   * so there's always a fallback path. Best-effort: if it times out (offline / relay
   * unreachable) we still serve on whatever direct addresses we have. */
  (void)endpoint_online(&ep, 10000);

  EndpointAddr_t addr = endpoint_addr_default();
  if (endpoint_addr(&ep, &addr) != ENDPOINT_RESULT_OK) {
    endpoint_close(ep);
    return MB_FAIL(MB_ERR_IO, "iroh: could not read endpoint address");
  }
  char *addr_str = endpoint_addr_as_str(&addr);
  char *key_str  = public_key_as_base32(&addr.id);
  snprintf(out_addr, addr_n, "%s", addr_str ? addr_str : "");
  snprintf(out_key,  key_n,  "%s", key_str  ? key_str  : "");
  if (addr_str) rust_free_string(addr_str);
  if (key_str)  rust_free_string(key_str);
  endpoint_addr_free(addr);

  mb_share_endpoint *h = calloc(1, sizeof *h);
  if (!h) { endpoint_close(ep); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  h->ep = ep;
  *out = h;
  return MB_OK;
}

mb_err mb_share_iroh_accept(mb_share_endpoint *ep, mb_share_transport *t) {
  Connection_t *conn = connection_default();
  Vec_uint8_t alpn_out = rust_buffer_alloc(0);
  int r = endpoint_accept_any(&ep->ep, &alpn_out, &conn);
  rust_buffer_free(alpn_out);
  if (r != ENDPOINT_RESULT_OK) { connection_free(conn); return MB_FAIL(MB_ERR_IO, "iroh: accept failed"); }

  SendStream_t *send = send_stream_default();
  RecvStream_t *recv = recv_stream_default();
  if (connection_accept_bi(&conn, &send, &recv) != ENDPOINT_RESULT_OK) {
    send_stream_free(send); recv_stream_free(recv); connection_free(conn);
    return MB_FAIL(MB_ERR_IO, "iroh: accept_bi failed");
  }
  iroh_ctx *c = calloc(1, sizeof *c);
  if (!c) { send_stream_free(send); recv_stream_free(recv); connection_free(conn); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  c->ep = ep->ep; c->owns_ep = 0; c->conn = conn; c->send = send; c->recv = recv;
  fill_transport(t, c);
  return MB_OK;
}

void mb_share_iroh_endpoint_free(mb_share_endpoint *ep) {
  if (!ep) return;
  if (ep->ep) endpoint_close(ep->ep);
  free(ep);
}

/* ---- guest ---- */
mb_err mb_share_iroh_connect(const char *addr_str, mb_share_transport *t) {
  EndpointAddr_t addr = endpoint_addr_default();
  if (endpoint_addr_from_string(addr_str, &addr) != 0)
    return MB_FAIL(MB_ERR_INVALID_ARG, "iroh: invalid host address");

  EndpointConfig_t cfg = endpoint_config_default();
  endpoint_config_add_alpn(&cfg, share_alpn());
  Endpoint_t *ep = endpoint_default();
  if (endpoint_bind(&cfg, NULL, NULL, &ep) != ENDPOINT_RESULT_OK) {
    endpoint_addr_free(addr);   /* not yet consumed — free it */
    return MB_FAIL(MB_ERR_IO, "iroh: bind failed");
  }
  /* Come online (home relay + a direct address) before dialing, so we can reach a
   * host that's only reachable via relay. Best-effort — connect still works on a
   * direct LAN path if the relay isn't available. */
  (void)endpoint_online(&ep, 10000);
  Connection_t *conn = connection_default();
  /* endpoint_connect takes `addr` BY VALUE and consumes it — do NOT free addr afterward
   * (that was a double-free). The client example likewise never frees a from_string addr. */
  int r = endpoint_connect(&ep, share_alpn(), addr, &conn);
  if (r != ENDPOINT_RESULT_OK) { connection_free(conn); endpoint_close(ep); return MB_FAIL(MB_ERR_IO, "iroh: connect failed"); }

  SendStream_t *send = send_stream_default();
  RecvStream_t *recv = recv_stream_default();
  if (connection_open_bi(&conn, &send, &recv) != ENDPOINT_RESULT_OK) {
    send_stream_free(send); recv_stream_free(recv); connection_free(conn); endpoint_close(ep);
    return MB_FAIL(MB_ERR_IO, "iroh: open_bi failed");
  }
  iroh_ctx *c = calloc(1, sizeof *c);
  if (!c) { send_stream_free(send); recv_stream_free(recv); connection_free(conn); endpoint_close(ep); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  c->ep = ep; c->owns_ep = 1; c->conn = conn; c->send = send; c->recv = recv;
  fill_transport(t, c);
  return MB_OK;
}

#endif /* MB_WITH_SHARE */
