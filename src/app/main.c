/*
 * Money Books — native shell (Phase 4, D1/D24; multi-company).
 *
 * Creates a system WKWebView window (via webview/webview), binds `mbInvoke` so the
 * React UI can call the C engine, and loads the bundled front-end. The bridge is the
 * SAME mb_api_dispatch surface the MCP layer uses, so UI and AI share one contract.
 *
 * Multi-company: one SQLite file = one company/book. `app.*` methods are handled here at the
 * shell level (they swap the active mb_store + maintain the on-disk book registry); everything
 * else dispatches to the current book. Launched with a path arg → open it; with no arg → open the
 * most-recently-used book, or none (the UI shows a launcher).
 *
 * Built only by `make app` (needs the webview header + WebKit/Cocoa). GUI on a logged-in session.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <limits.h>

#include "support/mb_compat.h"        /* getcwd/realpath/PATH_MAX (Windows) + <unistd.h> (POSIX) */

#include <webview/api.h>              /* C API (vendored via scripts/fetch_webview.sh) */
#include "vendor/cjson/cJSON.h"
#include "store/store.h"
#include "api/api.h"
#include "book/book.h"
#include "registry/registry.h"
#include "app/platform.h"            /* native host glue (exe path, dialogs) — one impl per OS */

#ifdef MB_WITH_SHARE
#include <stdatomic.h>
#include "support/mb_thread.h"     /* portable thread for the accept loop (pthread/Win32) */
#include "share/iroh.h"            /* host bind/accept + guest connect (iroh QUIC) */
#include "share/share.h"           /* mb_share_serve (host) / mb_share_call (guest) */

/* Short back-off in the accept loop. usleep is POSIX; Windows has Sleep (ms).
 * mb_thread.h already pulled <windows.h> (guarded) on Win32. */
#ifdef _WIN32
#  define mb_usleep(us) Sleep((DWORD)((us) / 1000))
#else
#  define mb_usleep(us) usleep(us)
#endif

/* Host-side sharing: the iroh endpoint is bound once per app session (so the key the owner
 * sends stays stable); share_start/share_stop just toggle `serving`, which the accept loop
 * consults — when off, newly accepted guests are closed immediately and no book data flows.
 * The endpoint and its read-only handle are released at process exit. Closing the endpoint
 * from the UI thread while the loop is blocked in accept would be a use-after-free (iroh's
 * endpoint_close consumes the endpoint), so we never tear it down mid-session — toggling the
 * gate is the safe equivalent and keeps the same node key for reconnects. */
struct app_share {
  mb_share_endpoint *ep;          /* bound listener (NULL until first share_start) */
  mb_store          *ro;          /* read-only book handle the serve loop owns */
  char               addr[1024];  /* dialable address string — what the owner sends */
  char               key[80];     /* base32 node id — a short fingerprint to confirm */
  mb_thread          thread;
  int                started;     /* endpoint bound + accept loop running */
  atomic_int         serving;     /* start→1, stop→0; the accept loop's gate */
  atomic_long        guests;      /* total guests served (status display) */
};
#endif

struct app_ctx {
  webview_t w;
  mb_store *s;                 /* current book, or NULL when on the launcher */
  char      book_path[1024];   /* path of the current book ("" if none) */
  char      reg_path[1024];    /* the registry JSON file */
#ifdef MB_WITH_SHARE
  struct app_share   share;        /* owner hosting a read-only share of c->s */
  mb_share_transport remote;       /* guest mode: live connection to a host */
  int                remote_active;
  char               remote_addr[1024];
#endif
};

/* ---- small JSON helpers ---- */
static char *json_take(cJSON *o) { char *s = cJSON_PrintUnformatted(o); cJSON_Delete(o); return s; }
static char *json_err(mb_err code, const char *msg) {
  cJSON *env = cJSON_CreateObject();
  cJSON *err = cJSON_AddObjectToObject(env, "error");
  cJSON_AddStringToObject(err, "code", mb_err_name(code));
  cJSON_AddStringToObject(err, "message", msg ? msg : mb_last_error()->message);
  return json_take(env);
}
static const char *sget(const cJSON *a, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Switch the active book: open `path`, close the previous store, record it in the registry. */
static mb_err open_book(struct app_ctx *c, const char *path) {
  mb_store *ns = NULL;
  MB_TRY(mb_store_open(path, &ns));
  if (c->s) mb_store_close(c->s);
  c->s = ns;
  snprintf(c->book_path, sizeof c->book_path, "%s", path);
  char nm[128] = "";
  (void)mb_book_company_name(ns, nm, sizeof nm);
  (void)mb_registry_touch(c->reg_path, path, nm[0] ? nm : NULL, (long long)time(NULL));
  if (c->w) webview_set_title(c->w, nm[0] ? nm : "Money Books");
  return MB_OK;
}

static char *ok_with_current(struct app_ctx *c) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", 1);
  cJSON_AddStringToObject(o, "path", c->book_path);
  return json_take(o);
}

/* Derive a default file path for a new company from its name (app dir / Safe_Name.sqlite). */
static void derive_path(const char *name, char *buf, size_t n) {
  char dir[1024];
  if (mb_registry_books_dir(dir, sizeof dir) != MB_OK) snprintf(dir, sizeof dir, ".");
  char safe[128]; size_t j = 0;
  for (size_t i = 0; name[i] && j < sizeof safe - 1; i++) {
    char ch = name[i];
    safe[j++] = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) ? ch : '_';
  }
  safe[j] = '\0';
  if (j == 0) snprintf(safe, sizeof safe, "book");
  snprintf(buf, n, "%s/%s.sqlite", dir, safe);
}

/* ---- live read-only sharing (Phase 7b-3) ---- */
#ifdef MB_WITH_SHARE
/* The serve loop's gate: nonzero while sharing is on. Checked per request, so stopping cuts
 * off an already-connected guest (on their next call/keepalive), not just new ones. */
static int share_gate_open(void *arg) {
  struct app_ctx *c = arg;
  return atomic_load(&c->share.serving);
}

/* Accept guests forever on the host endpoint. Serves each only while the gate is open;
 * when sharing is stopped, a connecting guest is accepted and immediately closed (clean EOF,
 * no data), and a connected guest is dropped on its next request. Runs detached for the app
 * session — the OS reclaims the endpoint at exit. */
static void *share_loop(void *arg) {
  struct app_ctx *c = arg;
  for (;;) {
    mb_share_transport t;
    if (mb_share_iroh_accept(c->share.ep, &t) != MB_OK) {
      mb_usleep(100000);   /* transient accept error (stray datagram); back off, keep listening */
      continue;
    }
    if (!atomic_load(&c->share.serving)) { t.close(t.ctx); continue; }  /* stopped → refuse */
    atomic_fetch_add(&c->share.guests, 1);
    (void)mb_share_serve_gated(c->share.ro, &t, share_gate_open, c);  /* returns on disconnect or stop */
    t.close(t.ctx);   /* close from THIS thread (safe) — guest sees the link drop */
  }
  return NULL;
}

/* Build the {sharing,available,address,key,guests} status the UI's Share panel renders. */
static char *share_status_json(struct app_ctx *c) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "available", 1);
  cJSON_AddBoolToObject(o, "sharing", atomic_load(&c->share.serving));
  if (c->share.started) {
    cJSON_AddStringToObject(o, "address", c->share.addr);
    cJSON_AddStringToObject(o, "key", c->share.key);
    cJSON_AddNumberToObject(o, "guests", (double)atomic_load(&c->share.guests));
  }
  return json_take(o);
}

static char *share_start(struct app_ctx *c) {
  if (!c->s || !c->book_path[0]) return json_err(MB_ERR_UNSUPPORTED, "open a book before sharing it");
  if (c->share.started) {                 /* re-enable on the existing endpoint (same key) */
    atomic_store(&c->share.serving, 1);
    return share_status_json(c);
  }
  mb_store *ro = NULL;
  mb_err re = mb_store_open_readonly(c->book_path, &ro);   /* the loop's own R/O handle */
  if (re != MB_OK) return json_err(re, NULL);
  mb_share_endpoint *ep = NULL;
  mb_err e = mb_share_iroh_bind(&ep, c->share.addr, sizeof c->share.addr,
                                c->share.key, sizeof c->share.key);
  if (e != MB_OK) { mb_store_close(ro); return json_err(e, NULL); }
  c->share.ep = ep;
  c->share.ro = ro;
  atomic_store(&c->share.serving, 1);
  if (mb_thread_create(&c->share.thread, share_loop, c) != 0) {
    mb_share_iroh_endpoint_free(ep); mb_store_close(ro);
    c->share.ep = NULL; c->share.ro = NULL;
    return json_err(MB_ERR_INTERNAL, "could not start the share thread");
  }
  mb_thread_detach(c->share.thread);
  c->share.started = 1;
  return share_status_json(c);
}

static char *share_stop(struct app_ctx *c) {
  atomic_store(&c->share.serving, 0);     /* the loop refuses new guests from here on */
  return share_status_json(c);
}

/* Guest: dial a host's address, enter remote read-only mode (any local book is closed so the
 * window is purely the shared view). Subsequent non-app calls forward over iroh in on_invoke. */
static char *share_connect(struct app_ctx *c, const cJSON *a) {
  const char *addr = a ? sget(a, "address") : NULL;
  if (!addr || !addr[0]) return json_err(MB_ERR_INVALID_ARG, "a share address is required");
  if (c->remote_active) return json_err(MB_ERR_UNSUPPORTED, "already connected to a shared book");
  mb_err e = mb_share_iroh_connect(addr, &c->remote);
  if (e != MB_OK) return json_err(e, NULL);
  c->remote_active = 1;
  snprintf(c->remote_addr, sizeof c->remote_addr, "%s", addr);
  if (c->s) { mb_store_close(c->s); c->s = NULL; }
  c->book_path[0] = '\0';
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", 1);
  return json_take(o);
}

static char *share_disconnect(struct app_ctx *c) {
  if (c->remote_active) { c->remote.close(c->remote.ctx); c->remote_active = 0; }
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", 1);
  return json_take(o);
}
#else  /* sharing compiled out: the methods exist but report unavailable */
static char *share_unavailable(void) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "available", 0);
  cJSON_AddBoolToObject(o, "sharing", 0);
  return json_take(o);
}
#endif

/* Shell-level `app.*` methods (book registry + active-store swap). Always returns a malloc'd JSON. */
static char *shell_dispatch(struct app_ctx *c, const char *method, const char *args_json) {
  cJSON *a = cJSON_Parse(args_json);
  char *out = NULL;

  if (!strcmp(method, "app.book_current")) {
    cJSON *o = cJSON_CreateObject();
#ifdef MB_WITH_SHARE
    if (c->remote_active) {                 /* guest mode: a live read-only view of a host's book */
      cJSON_AddNullToObject(o, "path");
      cJSON_AddStringToObject(o, "name", "Shared book");
      cJSON_AddBoolToObject(o, "read_only", 1);
      out = json_take(o);
      cJSON_Delete(a);
      return out;
    }
#endif
    if (c->s) {
      char nm[128] = ""; (void)mb_book_company_name(c->s, nm, sizeof nm);
      cJSON_AddStringToObject(o, "path", c->book_path);
      cJSON_AddStringToObject(o, "name", nm);
      cJSON_AddBoolToObject(o, "read_only", 0);
    } else {
      cJSON_AddNullToObject(o, "path");
    }
    out = json_take(o);

  } else if (!strcmp(method, "app.book_list")) {
    mb_book_ref *rows = NULL; int n = 0;
    mb_err e = mb_registry_list(c->reg_path, &rows, &n);
    if (e != MB_OK) { out = json_err(e, NULL); }
    else {
      cJSON *o = cJSON_CreateObject();
      cJSON *arr = cJSON_AddArrayToObject(o, "books");
      for (int i = 0; i < n; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "path", rows[i].path);
        cJSON_AddStringToObject(b, "name", rows[i].name);
        cJSON_AddNumberToObject(b, "last_opened", (double)rows[i].last_opened);
        cJSON_AddBoolToObject(b, "current", c->s && !strcmp(rows[i].path, c->book_path));
        cJSON_AddItemToArray(arr, b);
      }
      free(rows);
      out = json_take(o);
    }

  } else if (!strcmp(method, "app.book_open")) {
    const char *path = a ? sget(a, "path") : NULL;
    if (!path || !path[0]) out = json_err(MB_ERR_INVALID_ARG, "path required");
    else { mb_err e = open_book(c, path); out = (e == MB_OK) ? ok_with_current(c) : json_err(e, NULL); }

  } else if (!strcmp(method, "app.book_create")) {
    const char *name = a ? sget(a, "name") : NULL;
    const char *tmpl = a ? sget(a, "template") : NULL;
    const char *path = a ? sget(a, "path") : NULL;
    if (!name || !name[0]) { out = json_err(MB_ERR_INVALID_ARG, "company name required"); }
    else {
      char derived[1024];
      if (!path || !path[0]) { derive_path(name, derived, sizeof derived); path = derived; }
      mb_err e = mb_book_create(path, name, tmpl);
      if (e == MB_OK) e = open_book(c, path);
      out = (e == MB_OK) ? ok_with_current(c) : json_err(e, NULL);
    }

  } else if (!strcmp(method, "app.mcp_info")) {
    /* Everything the "Connect to Claude" dialog needs: the absolute MCP command, the book's
     * absolute path (one book = one company over MCP), and the Claude Desktop config location. */
    const char *path = a ? sget(a, "path") : NULL;
    if (!path || !path[0]) { out = json_err(MB_ERR_INVALID_ARG, "path required"); }
    else {
      char cmd[PATH_MAX]; mb_platform_mcp_binary_path(cmd, sizeof cmd);
      char real[PATH_MAX]; const char *bookabs = realpath(path, real) ? real : path;
      char cfg[PATH_MAX]; mb_platform_claude_config_path(cfg, sizeof cfg);
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "command", cmd);
      cJSON_AddStringToObject(o, "book_path", bookabs);
      cJSON_AddStringToObject(o, "config_path", cfg);
      out = json_take(o);
    }

  } else if (!strcmp(method, "app.book_forget")) {
    const char *path = a ? sget(a, "path") : NULL;
    if (!path || !path[0]) {
      out = json_err(MB_ERR_INVALID_ARG, "path required");
    } else {
      mb_err e = mb_registry_forget(c->reg_path, path);
      if (e != MB_OK) out = json_err(e, NULL);
      else { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", 1); out = json_take(o); }
    }

  } else if (!strcmp(method, "app.share_start")) {
#ifdef MB_WITH_SHARE
    out = share_start(c);
#else
    out = share_unavailable();
#endif
  } else if (!strcmp(method, "app.share_stop")) {
#ifdef MB_WITH_SHARE
    out = share_stop(c);
#else
    out = share_unavailable();
#endif
  } else if (!strcmp(method, "app.share_status")) {
#ifdef MB_WITH_SHARE
    out = share_status_json(c);
#else
    out = share_unavailable();
#endif
  } else if (!strcmp(method, "app.share_connect")) {
#ifdef MB_WITH_SHARE
    out = share_connect(c, a);
#else
    out = share_unavailable();
#endif
  } else if (!strcmp(method, "app.share_disconnect")) {
#ifdef MB_WITH_SHARE
    out = share_disconnect(c);
#else
    out = share_unavailable();
#endif

  } else {
    out = json_err(MB_ERR_UNSUPPORTED, "unknown app method");
  }

  cJSON_Delete(a);
  return out ? out : json_err(MB_ERR_INTERNAL, "no result");
}

/* JS calls window.mbInvoke(method, argsJson). webview passes req as a JSON array
 * ["method","argsJson"]; we dispatch and return the JSON result string. */
static void on_invoke(const char *id, const char *req, void *arg) {
  struct app_ctx *c = arg;
  webview_t w = c->w;

  cJSON *params = cJSON_Parse(req);
  const char *method = "";
  const char *args = "{}";
  if (cJSON_IsArray(params)) {
    cJSON *m = cJSON_GetArrayItem(params, 0);
    cJSON *a = cJSON_GetArrayItem(params, 1);
    if (cJSON_IsString(m)) method = m->valuestring;
    if (cJSON_IsString(a)) args = a->valuestring;
  }

  /* shell-level book management (works with or without a book open) */
  if (!strncmp(method, "app.", 4)) {
    char *r = shell_dispatch(c, method, args);
    webview_return(w, id, 0, r);
    free(r);
    cJSON_Delete(params);
    return;
  }

#ifdef MB_WITH_SHARE
  /* guest mode: forward every engine call over iroh to the host's read-only dispatch.
   * webview callbacks are serialized on the UI thread, so calls never interleave on the
   * single shared stream. A transport failure means the host stopped or the link dropped —
   * surface it and drop back to disconnected so the UI can return to the launcher. */
  if (c->remote_active) {
    char *result = NULL;
    mb_err e = mb_share_call(&c->remote, method, args, &result);
    if (e != MB_OK) {
      c->remote.close(c->remote.ctx);
      c->remote_active = 0;
      char *r = json_err(MB_ERR_IO, "lost connection to the shared book");
      webview_return(w, id, 0, r);
      free(r); free(result); cJSON_Delete(params);
      return;
    }
    webview_return(w, id, 0, result ? result : "{}");
    free(result);
    cJSON_Delete(params);
    return;
  }
#endif

  /* every other method needs an open book */
  if (!c->s) {
    char *r = json_err(MB_ERR_UNSUPPORTED, "no book is open");
    webview_return(w, id, 0, r);
    free(r);
    cJSON_Delete(params);
    return;
  }

  char *result = NULL;
  (void)mb_api_dispatch(c->s, method, args, &result);  /* fast engine calls stay synchronous */
  webview_return(w, id, 0, result ? result : "{\"error\":{\"code\":\"MB_ERR_INTERNAL\",\"message\":\"no result\"}}");
  free(result);
  cJSON_Delete(params);
}

/* JS calls window.mbSaveFile(filename, content) → native Save dialog (NSSavePanel).
 * webview passes req as ["filename","content"]. Resolves with {ok, path} on save,
 * {ok:false, cancelled:true} if dismissed; rejects the JS promise on a write error. */
static void on_save_file(const char *id, const char *req, void *arg) {
  struct app_ctx *c = arg;
  cJSON *params = cJSON_Parse(req);
  const char *filename = NULL, *content = NULL;
  if (cJSON_IsArray(params)) {
    cJSON *f = cJSON_GetArrayItem(params, 0);
    cJSON *t = cJSON_GetArrayItem(params, 1);
    if (cJSON_IsString(f)) filename = f->valuestring;
    if (cJSON_IsString(t)) content = t->valuestring;
  }

  char buf[1024] = "";
  int rc = mb_platform_save_file(filename, content, buf, sizeof buf);

  cJSON *o = cJSON_CreateObject();
  if (rc == 1) {
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddStringToObject(o, "path", buf);
  } else if (rc == 0) {
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddBoolToObject(o, "cancelled", 1);
  } else {
    cJSON_AddStringToObject(o, "message", buf[0] ? buf : "save failed");
  }
  char *r = json_take(o);
  webview_return(c->w, id, rc < 0 ? 1 : 0, r);  /* status!=0 → reject the JS promise */
  free(r);
  cJSON_Delete(params);
}

int main(int argc, char **argv) {
  static struct app_ctx c;
  if (mb_registry_default_path(c.reg_path, sizeof c.reg_path) != MB_OK)
    fprintf(stderr, "registry warning: %s\n", mb_last_error()->message);

  if (argc > 1) {
    if (open_book(&c, argv[1]) != MB_OK)
      fprintf(stderr, "failed to open book '%s': %s\n", argv[1], mb_last_error()->message);
  } else {
    /* no arg: open the most-recently-used book, else fall through to the launcher */
    mb_book_ref *rows = NULL; int n = 0;
    if (mb_registry_list(c.reg_path, &rows, &n) == MB_OK && n > 0)
      (void)open_book(&c, rows[0].path);   /* if it fails (file moved), c.s stays NULL → launcher */
    free(rows);
  }

  /* debug=1 enables the Web Inspector (right-click → Inspect Element) for development */
  webview_t w = webview_create(1, NULL);
  c.w = w;

  /* Standard edit/quit shortcuts (⌘/Ctrl + C/V/Q). The webview ships no menu, so
   * copy/paste in text fields and ⌘Q wouldn't work without this. */
  mb_platform_install_menu("Money Books");

  char title[160] = "Money Books";
  if (c.s) { char nm[128] = ""; if (mb_book_company_name(c.s, nm, sizeof nm) == MB_OK && nm[0]) snprintf(title, sizeof title, "%s — Money Books", nm); }
  webview_set_title(w, title);
  webview_set_size(w, 1100, 760, WEBVIEW_HINT_NONE);
  webview_bind(w, "mbInvoke", on_invoke, &c);
  webview_bind(w, "mbSaveFile", on_save_file, &c);  /* native Save dialog for CSV exports */

  /* Load the built UI. Override with MB_UI_URL (e.g. http://localhost:5173 during dev). */
  const char *url = getenv("MB_UI_URL");
  char filebuf[1024];
  if (!url) {
    char cwd[768];
    if (getcwd(cwd, sizeof cwd)) {
      snprintf(filebuf, sizeof filebuf, "file://%s/ui/dist/index.html", cwd);
      url = filebuf;
    } else {
      url = "file://ui/dist/index.html";
    }
  }
  webview_navigate(w, url);

  webview_run(w);
  webview_destroy(w);
#ifdef MB_WITH_SHARE
  if (c.remote_active) c.remote.close(c.remote.ctx);
  /* the detached share loop + its read-only handle + endpoint are reclaimed at process exit;
   * we never free the endpoint here (it would race the loop blocked in accept). */
#endif
  if (c.s) mb_store_close(c.s);
  return 0;
}
