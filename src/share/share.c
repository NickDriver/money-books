#include "share.h"

#include <stdlib.h>
#include <string.h>
#include "../api/api.h"
#include "vendor/cjson/cJSON.h"

static char *err_envelope(const char *code, const char *msg) {
  cJSON *env = cJSON_CreateObject();
  cJSON *err = cJSON_AddObjectToObject(env, "error");
  cJSON_AddStringToObject(err, "code", code);
  cJSON_AddStringToObject(err, "message", msg);
  char *s = cJSON_PrintUnformatted(env);
  cJSON_Delete(env);
  return s;
}

/* Dispatch one already-received request frame and send its response. Takes ownership of
 * `req` (frees it). Shared by handle_one and the gated serve loop. */
static mb_err respond_to(mb_store *ro, mb_share_transport *t, void *req, size_t rlen) {
  cJSON *j = cJSON_ParseWithLength((const char *)req, rlen);
  free(req);

  char *resp = NULL;
  const cJSON *m = j ? cJSON_GetObjectItemCaseSensitive(j, "method") : NULL;
  if (!cJSON_IsString(m) || !m->valuestring) {
    resp = err_envelope("MB_ERR_PARSE", "malformed request frame");
  } else {
    cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "args");
    char *args = a ? cJSON_PrintUnformatted(a) : NULL;
    (void)mb_api_dispatch_guest(ro, m->valuestring, args ? args : "{}", &resp);
    free(args);
  }
  cJSON_Delete(j);
  if (!resp) resp = err_envelope("MB_ERR_INTERNAL", "no response");
  if (!resp) return MB_FAIL(MB_ERR_INTERNAL, "oom");

  mb_err e = t->send(t->ctx, resp, strlen(resp));
  free(resp);
  return e;
}

mb_err mb_share_handle_one(mb_store *ro, mb_share_transport *t) {
  void *req = NULL; size_t rlen = 0;
  MB_TRY(t->recv(t->ctx, &req, &rlen));   /* MB_ERR_IO at EOF — caller's loop ends */
  return respond_to(ro, t, req, rlen);
}

mb_err mb_share_serve(mb_store *ro, mb_share_transport *t) {
  for (;;) {
    mb_err e = mb_share_handle_one(ro, t);
    if (e != MB_OK) return e;   /* MB_ERR_IO on close = normal end of session */
  }
}

mb_err mb_share_serve_gated(mb_store *ro, mb_share_transport *t,
                            int (*open)(void *), void *octx) {
  for (;;) {
    void *req = NULL; size_t rlen = 0;
    mb_err e = t->recv(t->ctx, &req, &rlen);
    if (e != MB_OK) return e;                          /* guest disconnected (EOF) */
    /* Check the gate AFTER a frame arrives but BEFORE answering it: if the owner stopped
     * sharing while this guest was connected, drop the request unanswered and end the
     * session. The caller closes the transport from THIS (the serving) thread — safe,
     * unlike closing a connection another thread is blocked reading. */
    if (open && !open(octx)) { free(req); return MB_OK; }
    e = respond_to(ro, t, req, rlen);
    if (e != MB_OK) return e;
  }
}

mb_err mb_share_call(mb_share_transport *t, const char *method, const char *args_json, char **out) {
  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "method", method);
  cJSON *a = (args_json && args_json[0]) ? cJSON_Parse(args_json) : NULL;
  cJSON_AddItemToObject(req, "args", a ? a : cJSON_CreateObject());
  char *reqs = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if (!reqs) return MB_FAIL(MB_ERR_INTERNAL, "oom");

  mb_err e = t->send(t->ctx, reqs, strlen(reqs));
  free(reqs);
  if (e != MB_OK) return e;

  void *resp = NULL; size_t len = 0;
  MB_TRY(t->recv(t->ctx, &resp, &len));
  char *s = malloc(len + 1);
  if (!s) { free(resp); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  memcpy(s, resp, len);
  s[len] = '\0';
  free(resp);
  *out = s;
  return MB_OK;
}
