#include "llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "vendor/cjson/cJSON.h"

typedef struct {
  char *base_url;
  char *api_key;
  char *model;
} openai_ctx;

static const char *SYSTEM_PROMPT =
  "You are the bookkeeping assistant inside Money Books, a double-entry accounting app. "
  "Use the provided tools to read and modify the user's books. All monetary amounts are integer "
  "minor units (cents) — e.g. $12.34 is 1234. Some names are pseudonymized as tokens like Client_1 "
  "or Account_1; treat them as opaque identifiers and use them as-is. Be concise and confirm what "
  "you did.";

/* ---- libcurl response buffer ---- */
struct buf { char *data; size_t len; };
static size_t on_write(char *ptr, size_t size, size_t nmemb, void *ud) {
  struct buf *b = ud;
  size_t add = size * nmemb;
  char *np = realloc(b->data, b->len + add + 1);
  if (!np) return 0;
  b->data = np;
  memcpy(b->data + b->len, ptr, add);
  b->len += add;
  b->data[b->len] = '\0';
  return add;
}

/* our protocol messages -> OpenAI messages (system prepended) */
static cJSON *to_openai_messages(const cJSON *msgs) {
  cJSON *arr = cJSON_CreateArray();
  cJSON *sys = cJSON_CreateObject();
  cJSON_AddStringToObject(sys, "role", "system");
  cJSON_AddStringToObject(sys, "content", SYSTEM_PROMPT);
  cJSON_AddItemToArray(arr, sys);

  const cJSON *m;
  cJSON_ArrayForEach(m, msgs) {
    const cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
    const cJSON *tcs = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "role", cJSON_IsString(role) ? role->valuestring : "user");
    if (cJSON_IsArray(tcs)) {                     /* assistant tool-call turn */
      cJSON_AddNullToObject(o, "content");
      cJSON *out_tcs = cJSON_AddArrayToObject(o, "tool_calls");
      const cJSON *tc;
      cJSON_ArrayForEach(tc, tcs) {
        cJSON *ot = cJSON_CreateObject();
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(tc, "id");
        cJSON_AddStringToObject(ot, "id", cJSON_IsString(id) ? id->valuestring : "call_0");
        cJSON_AddStringToObject(ot, "type", "function");
        cJSON *fn = cJSON_AddObjectToObject(ot, "function");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(tc, "name");
        cJSON_AddStringToObject(fn, "name", cJSON_IsString(name) ? name->valuestring : "");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(tc, "arguments");
        char *as = args ? cJSON_PrintUnformatted(args) : NULL;
        cJSON_AddStringToObject(fn, "arguments", as ? as : "{}");
        free(as);
        cJSON_AddItemToArray(out_tcs, ot);
      }
    } else {
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
      cJSON_AddStringToObject(o, "content", cJSON_IsString(content) ? content->valuestring : "");
      const cJSON *tcid = cJSON_GetObjectItemCaseSensitive(m, "tool_call_id");
      if (cJSON_IsString(tcid)) cJSON_AddStringToObject(o, "tool_call_id", tcid->valuestring);
    }
    cJSON_AddItemToArray(arr, o);
  }
  return arr;
}

static cJSON *to_openai_tools(const cJSON *tools) {
  cJSON *arr = cJSON_CreateArray();
  const cJSON *t;
  cJSON_ArrayForEach(t, tools) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "function");
    cJSON *fn = cJSON_AddObjectToObject(o, "function");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(t, "name");
    const cJSON *desc = cJSON_GetObjectItemCaseSensitive(t, "description");
    cJSON_AddStringToObject(fn, "name", cJSON_IsString(name) ? name->valuestring : "");
    cJSON_AddStringToObject(fn, "description", cJSON_IsString(desc) ? desc->valuestring : "");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(t, "parameters");
    if (params) cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(params, 1));
    else cJSON_AddItemToObject(fn, "parameters", cJSON_Parse("{\"type\":\"object\"}"));
    cJSON_AddItemToArray(arr, o);
  }
  return arr;
}

/* OpenAI response -> our protocol {tool_calls|text} */
static char *from_openai(const char *resp_json) {
  cJSON *root = cJSON_Parse(resp_json);
  if (!root) return NULL;
  cJSON *out = cJSON_CreateObject();

  cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
  cJSON *msg = choices ? cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(choices, 0), "message") : NULL;
  cJSON *tcs = msg ? cJSON_GetObjectItemCaseSensitive(msg, "tool_calls") : NULL;

  if (cJSON_IsArray(tcs) && cJSON_GetArraySize(tcs) > 0) {
    cJSON *arr = cJSON_AddArrayToObject(out, "tool_calls");
    cJSON *tc;
    cJSON_ArrayForEach(tc, tcs) {
      cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
      cJSON *o = cJSON_CreateObject();
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(tc, "id");
      cJSON_AddStringToObject(o, "id", cJSON_IsString(id) ? id->valuestring : "call_0");
      const cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
      cJSON_AddStringToObject(o, "name", cJSON_IsString(name) ? name->valuestring : "");
      const cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
      cJSON *parsed = (cJSON_IsString(args)) ? cJSON_Parse(args->valuestring) : NULL;
      cJSON_AddItemToObject(o, "arguments", parsed ? parsed : cJSON_CreateObject());
      cJSON_AddItemToArray(arr, o);
    }
  } else {
    const cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
    /* surface API errors as text so they reach the user */
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    const char *text = cJSON_IsString(content) ? content->valuestring
                     : (err ? "(provider error)" : "");
    cJSON_AddStringToObject(out, "text", text);
  }
  char *s = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  cJSON_Delete(root);
  return s;
}

static char *openai_complete(void *ctx, const char *request_json) {
  openai_ctx *c = ctx;
  cJSON *req = cJSON_Parse(request_json);
  if (!req) return NULL;

  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "model", c->model);
  cJSON_AddItemToObject(body, "messages", to_openai_messages(cJSON_GetObjectItemCaseSensitive(req, "messages")));
  cJSON_AddItemToObject(body, "tools", to_openai_tools(cJSON_GetObjectItemCaseSensitive(req, "tools")));
  cJSON_AddStringToObject(body, "tool_choice", "auto");
  cJSON_AddBoolToObject(body, "stream", 0);
  char *body_str = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  cJSON_Delete(req);

  CURL *curl = curl_easy_init();
  if (!curl) { free(body_str); return NULL; }
  char url[512], auth[512];
  snprintf(url, sizeof url, "%s/chat/completions", c->base_url);
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->api_key);
  struct curl_slist *h = NULL;
  h = curl_slist_append(h, "Content-Type: application/json");
  h = curl_slist_append(h, auth);

  struct buf b = {0};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(h);
  curl_easy_cleanup(curl);
  free(body_str);

  char *result = NULL;
  if (rc == CURLE_OK && b.data) result = from_openai(b.data);
  free(b.data);
  return result;
}

mb_err mb_llm_openai_provider(const char *base_url, const char *api_key, const char *model,
                              mb_llm_provider *out) {
  if (!api_key || !api_key[0]) return MB_FAIL(MB_ERR_PERMISSION, "no API key configured");
  openai_ctx *c = calloc(1, sizeof *c);
  if (!c) return MB_FAIL(MB_ERR_INTERNAL, "oom");
  c->base_url = strdup(base_url && base_url[0] ? base_url : "https://api.openai.com/v1");
  c->api_key = strdup(api_key);
  c->model = strdup(model && model[0] ? model : "gpt-4o-mini");
  out->complete = openai_complete;
  out->ctx = c;
  return MB_OK;
}

mb_err mb_llm_provider_from_env(mb_llm_provider *out) {
  return mb_llm_openai_provider(getenv("MB_LLM_BASE_URL"), getenv("MB_LLM_API_KEY"),
                                getenv("MB_LLM_MODEL"), out);
}

void mb_llm_provider_free(mb_llm_provider *p) {
  if (!p || !p->ctx) return;
  openai_ctx *c = p->ctx;
  free(c->base_url); free(c->api_key); free(c->model); free(c);
  p->ctx = NULL;
}
