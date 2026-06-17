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
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include <webview/api.h>              /* C API (vendored via scripts/fetch_webview.sh) */
#include "vendor/cjson/cJSON.h"
#include "store/store.h"
#include "api/api.h"
#include "book/book.h"
#include "registry/registry.h"

struct app_ctx {
  webview_t w;
  mb_store *s;                 /* current book, or NULL when on the launcher */
  char      book_path[1024];   /* path of the current book ("" if none) */
  char      reg_path[1024];    /* the registry JSON file */
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

/* Shell-level `app.*` methods (book registry + active-store swap). Always returns a malloc'd JSON. */
static char *shell_dispatch(struct app_ctx *c, const char *method, const char *args_json) {
  cJSON *a = cJSON_Parse(args_json);
  char *out = NULL;

  if (!strcmp(method, "app.book_current")) {
    cJSON *o = cJSON_CreateObject();
    if (c->s) {
      char nm[128] = ""; (void)mb_book_company_name(c->s, nm, sizeof nm);
      cJSON_AddStringToObject(o, "path", c->book_path);
      cJSON_AddStringToObject(o, "name", nm);
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

  } else if (!strcmp(method, "app.book_forget")) {
    const char *path = a ? sget(a, "path") : NULL;
    if (!path || !path[0]) {
      out = json_err(MB_ERR_INVALID_ARG, "path required");
    } else {
      mb_err e = mb_registry_forget(c->reg_path, path);
      if (e != MB_OK) out = json_err(e, NULL);
      else { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", 1); out = json_take(o); }
    }

  } else {
    out = json_err(MB_ERR_UNSUPPORTED, "unknown app method");
  }

  cJSON_Delete(a);
  return out ? out : json_err(MB_ERR_INTERNAL, "no result");
}

/* ---- async path for slow calls (agent.send) so the UI thread never blocks ---- */
struct agent_job { webview_t w; mb_store *s; char *id; char *args; };
struct agent_ret { char *id; char *result; };

static void deliver_ret(webview_t w, void *p) {   /* runs on the main thread */
  struct agent_ret *r = p;
  webview_return(w, r->id, 0, r->result);
  free(r->id); free(r->result); free(r);
}
static void *run_agent(void *p) {
  struct agent_job *j = p;
  char *res = NULL;
  (void)mb_api_dispatch(j->s, "agent.send", j->args, &res);   /* the slow, network-bound call */
  struct agent_ret *r = malloc(sizeof *r);
  r->id = j->id;                                              /* ownership transferred */
  r->result = res ? res : strdup("{\"error\":{\"code\":\"MB_ERR_INTERNAL\",\"message\":\"no result\"}}");
  webview_dispatch(j->w, deliver_ret, r);                     /* hop back to the main thread */
  free(j->args);
  free(j);
  return NULL;
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

  /* every other method needs an open book */
  if (!c->s) {
    char *r = json_err(MB_ERR_UNSUPPORTED, "no book is open");
    webview_return(w, id, 0, r);
    free(r);
    cJSON_Delete(params);
    return;
  }

  /* agent.send can take many seconds (LLM round-trips) — run it off the UI thread so the
   * window stays responsive and the chat's typing indicator can animate. */
  if (!strcmp(method, "agent.send")) {
    struct agent_job *j = malloc(sizeof *j);
    j->w = w; j->s = c->s; j->id = strdup(id); j->args = strdup(args);
    pthread_t t;
    if (pthread_create(&t, NULL, run_agent, j) == 0) {
      pthread_detach(t);
      cJSON_Delete(params);
      return;                                   /* webview_return happens later, from the worker */
    }
    free(j->id); free(j->args); free(j);        /* fall through to sync on thread-create failure */
  }

  char *result = NULL;
  (void)mb_api_dispatch(c->s, method, args, &result);  /* fast engine calls stay synchronous */
  webview_return(w, id, 0, result ? result : "{\"error\":{\"code\":\"MB_ERR_INTERNAL\",\"message\":\"no result\"}}");
  free(result);
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

  char title[160] = "Money Books";
  if (c.s) { char nm[128] = ""; if (mb_book_company_name(c.s, nm, sizeof nm) == MB_OK && nm[0]) snprintf(title, sizeof title, "%s — Money Books", nm); }
  webview_set_title(w, title);
  webview_set_size(w, 1100, 760, WEBVIEW_HINT_NONE);
  webview_bind(w, "mbInvoke", on_invoke, &c);

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
  if (c.s) mb_store_close(c.s);
  return 0;
}
