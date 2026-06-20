#ifndef MB_SHARE_TRANSPORT_H
#define MB_SHARE_TRANSPORT_H
/*
 * Money Books — share transport (Phase 7b).
 *
 * A bidirectional, message-framed channel between a share *host* (the owner serving a
 * read-only book) and a *guest* (the viewer). Every call sends or receives one complete
 * frame — a JSON document. share.c is written against this interface only, so the same
 * host/guest protocol runs over either implementation:
 *   - in-memory loopback (this file's pair helper) — tests and a same-process demo,
 *   - iroh QUIC (Phase 7b-2, transport_iroh.c) — real host↔guest over the internet.
 */
#include <stddef.h>
#include "../support/mb_error.h"

typedef struct mb_share_transport {
  mb_err (*send)(void *ctx, const void *buf, size_t len);  /* send one frame              */
  mb_err (*recv)(void *ctx, void **buf, size_t *len);      /* recv one frame; *buf malloc'd,
                                                              caller frees; MB_ERR_IO at EOF */
  void   (*close)(void *ctx);
  void *ctx;
} mb_share_transport;

/* Wire two transports to each other in-memory. Both ends are thread-safe blocking channels,
 * so a guest thread can mb_share_call() while a host thread runs mb_share_serve(). Free the
 * pair with mb_share_loopback_free(state) once both ends are done. */
mb_err mb_share_loopback_pair(mb_share_transport *host, mb_share_transport *guest,
                              void **state) MB_MUST_CHECK;
void   mb_share_loopback_free(void *state);

#endif /* MB_SHARE_TRANSPORT_H */
