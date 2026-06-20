#ifndef MB_SHARE_H
#define MB_SHARE_H
/*
 * Money Books — share protocol (Phase 7b).
 *
 * Host↔guest request/response over an mb_share_transport. The guest sends
 * {"method":..,"args":..} frames; the host answers each through mb_api_dispatch_guest
 * (read-only allowlist) against a read-only book and sends the JSON result back. This is
 * the whole "live read-only book sharing" protocol, transport-independent: loopback drives
 * it in tests, iroh drives it across the internet (Phase 7b-2).
 */
#include "transport.h"
#include "../store/store.h"

/* Serve one request: recv a frame, dispatch it read-only against `ro`, send the result.
 * Returns MB_ERR_IO when the transport is closed (no frame to read). */
mb_err mb_share_handle_one(mb_store *ro, mb_share_transport *t) MB_MUST_CHECK;

/* Serve requests until the transport closes (run this on the host's per-guest thread). */
mb_err mb_share_serve(mb_store *ro, mb_share_transport *t) MB_MUST_CHECK;

/* Like mb_share_serve, but checks `open(octx)` after each request arrives and before
 * answering it: when it returns 0 (the owner stopped sharing), the request is dropped
 * unanswered and the loop returns MB_OK so the caller can close the connection from this
 * thread. This makes "Stop sharing" cut off an already-connected guest on their next call
 * (or next keepalive), not just refuse new ones. */
mb_err mb_share_serve_gated(mb_store *ro, mb_share_transport *t,
                            int (*open)(void *), void *octx) MB_MUST_CHECK;

/* Guest side: send one request, return the host's JSON response (malloc'd, NUL-terminated;
 * caller frees). The response may be a result or an {"error":..} envelope — that's an
 * application outcome, not a transport failure, so this returns MB_OK on a successful
 * round trip. This is exactly what a guest's mbInvoke forwards. */
mb_err mb_share_call(mb_share_transport *t, const char *method, const char *args_json,
                     char **out) MB_MUST_CHECK;

#endif /* MB_SHARE_H */
