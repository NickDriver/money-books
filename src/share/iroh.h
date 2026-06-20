#ifndef MB_SHARE_IROH_H
#define MB_SHARE_IROH_H
/*
 * Money Books — iroh transport entry points (Phase 7b-2).
 *
 * Real host↔guest QUIC over iroh, dial-by-key. Implemented in transport_iroh.c (the only
 * file that includes irohnet.h / links the Rust staticlib). These produce an
 * mb_share_transport the share protocol (share.c) runs over unchanged.
 *
 * Only built when compiled with -DMB_WITH_SHARE (the share/app build); pure-C targets never
 * reference these, so they stay Rust-free. Declarations here use no iroh types, so this
 * header is safe to include anywhere.
 */
#include "transport.h"

typedef struct mb_share_endpoint mb_share_endpoint;  /* a host listener (opaque) */

/* Host: bind an iroh endpoint advertising the share ALPN. On success *out holds the
 * endpoint; out_addr (the dialable address string the guest needs — "send a key") and
 * out_key (the base32 node id, a short fingerprint for confirmation) are filled. */
mb_err mb_share_iroh_bind(mb_share_endpoint **out,
                          char *out_addr, size_t addr_n,
                          char *out_key, size_t key_n) MB_MUST_CHECK;

/* Host: block until a guest connects and opens a stream, then fill `t` with its transport.
 * Call repeatedly to serve successive guests. */
mb_err mb_share_iroh_accept(mb_share_endpoint *ep, mb_share_transport *t) MB_MUST_CHECK;

void mb_share_iroh_endpoint_free(mb_share_endpoint *ep);

/* Guest: dial the host's address string, open a stream, fill `t`. The transport owns its
 * own endpoint and closes everything on t->close(). */
mb_err mb_share_iroh_connect(const char *addr_str, mb_share_transport *t) MB_MUST_CHECK;

#endif /* MB_SHARE_IROH_H */
