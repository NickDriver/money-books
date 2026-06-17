#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vendor/cjson/cJSON.h"
#include "../api/api.h"
#include "../redact/redact.h"

#define AGENT_MAX_ITERS 8

static const char *jstr(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* tools advertised to the model (names == api methods executed via mb_api_dispatch).
 * {name, description, JSON-Schema parameters} */
static cJSON *build_tools(void) {
  static const char *T[][3] = {
    {"account.list", "List accounts and categories.",
     "{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\"},\"role\":{\"type\":\"string\"},\"active_only\":{\"type\":\"boolean\"}}}"},
    {"report.pnl", "Profit & loss over a date range (YYYY-MM-DD; omit for all-time).",
     "{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}}}"},
    {"report.balance_sheet", "Balance sheet as of a date.",
     "{\"type\":\"object\",\"properties\":{\"as_of\":{\"type\":\"string\"}}}"},
    {"report.cash_flow", "Cash flow over a date range.",
     "{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}}}"},
    {"income.record", "Record income: debit a deposit account, credit an income category.",
     "{\"type\":\"object\",\"properties\":{\"date\":{\"type\":\"string\"},\"amount\":{\"type\":\"integer\"},\"deposit_account_id\":{\"type\":\"string\"},\"category_id\":{\"type\":\"string\"},\"memo\":{\"type\":\"string\"}},\"required\":[\"date\",\"amount\",\"deposit_account_id\",\"category_id\"]}"},
    {"expense.record", "Record an expense: debit an expense category, credit a payment account.",
     "{\"type\":\"object\",\"properties\":{\"date\":{\"type\":\"string\"},\"amount\":{\"type\":\"integer\"},\"pay_account_id\":{\"type\":\"string\"},\"category_id\":{\"type\":\"string\"},\"memo\":{\"type\":\"string\"}},\"required\":[\"date\",\"amount\",\"pay_account_id\",\"category_id\"]}"},
    {"invoice.create", "Create a draft invoice for a counterparty.",
     "{\"type\":\"object\",\"properties\":{\"counterparty_id\":{\"type\":\"string\"},\"number\":{\"type\":\"string\"},\"due_date\":{\"type\":\"string\"}},\"required\":[\"counterparty_id\"]}"},
    {"invoice.issue", "Issue a draft invoice (posts Dr AR / Cr income).",
     "{\"type\":\"object\",\"properties\":{\"invoice_id\":{\"type\":\"string\"},\"issue_date\":{\"type\":\"string\"}},\"required\":[\"invoice_id\",\"issue_date\"]}"},
    {"payment.record", "Record a payment against an invoice or bill.",
     "{\"type\":\"object\",\"properties\":{\"date\":{\"type\":\"string\"},\"amount\":{\"type\":\"integer\"},\"cash_account_id\":{\"type\":\"string\"},\"target\":{\"type\":\"string\"},\"target_id\":{\"type\":\"string\"}},\"required\":[\"date\",\"amount\",\"cash_account_id\",\"target\",\"target_id\"]}"},
  };
  cJSON *arr = cJSON_CreateArray();
  for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", T[i][0]);
    cJSON_AddStringToObject(t, "description", T[i][1]);
    cJSON_AddItemToObject(t, "parameters", cJSON_Parse(T[i][2]));
    cJSON_AddItemToArray(arr, t);
  }
  return arr;
}

mb_err mb_agent_run(mb_store *s, const mb_llm_provider *prov, const char *user_msg, char **reply_out) {
  *reply_out = NULL;
  mb_redactor *r = NULL;
  MB_TRY(mb_redactor_create(s, &r));

  cJSON *messages = cJSON_CreateArray();
  cJSON *tools = build_tools();

  char *umsg = mb_redact(r, user_msg);                 /* redact before it ever leaves */
  cJSON *um = cJSON_CreateObject();
  cJSON_AddStringToObject(um, "role", "user");
  cJSON_AddStringToObject(um, "content", umsg ? umsg : user_msg);
  cJSON_AddItemToArray(messages, um);
  free(umsg);

  mb_err e = MB_OK;
  char *reply = NULL;

  for (int iter = 0; iter < AGENT_MAX_ITERS && !reply && e == MB_OK; iter++) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "messages", cJSON_Duplicate(messages, 1));
    cJSON_AddItemToObject(req, "tools", cJSON_Duplicate(tools, 1));
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    char *resp_str = prov->complete(prov->ctx, req_str);
    free(req_str);
    if (!resp_str) { e = MB_FAIL(MB_ERR_IO, "provider returned no response"); break; }
    cJSON *resp = cJSON_Parse(resp_str);
    free(resp_str);
    if (!resp) { e = MB_FAIL(MB_ERR_PARSE, "bad provider response"); break; }

    cJSON *tcs = cJSON_GetObjectItemCaseSensitive(resp, "tool_calls");
    if (cJSON_IsArray(tcs) && cJSON_GetArraySize(tcs) > 0) {
      /* echo the assistant's tool calls into the transcript */
      cJSON *am = cJSON_CreateObject();
      cJSON_AddStringToObject(am, "role", "assistant");
      cJSON_AddItemToObject(am, "tool_calls", cJSON_Duplicate(tcs, 1));
      cJSON_AddItemToArray(messages, am);

      cJSON *tc;
      cJSON_ArrayForEach(tc, tcs) {
        const char *name = jstr(tc, "name");
        cJSON *args = cJSON_GetObjectItemCaseSensitive(tc, "arguments");
        char *argstr = args ? cJSON_PrintUnformatted(args) : NULL;
        char *res = NULL;
        (void)mb_api_dispatch(s, name ? name : "", argstr ? argstr : "{}", &res);
        free(argstr);
        char *red = res ? mb_redact(r, res) : NULL;   /* redact tool results too (egress) */

        cJSON *tm = cJSON_CreateObject();
        cJSON_AddStringToObject(tm, "role", "tool");
        if (name) cJSON_AddStringToObject(tm, "name", name);
        const char *idv = jstr(tc, "id");
        if (idv) cJSON_AddStringToObject(tm, "tool_call_id", idv);
        cJSON_AddStringToObject(tm, "content", red ? red : (res ? res : "{}"));
        cJSON_AddItemToArray(messages, tm);
        free(res);
        free(red);
      }
      cJSON_Delete(resp);
      continue;
    }

    /* final answer */
    const char *text = jstr(resp, "text");
    reply = text ? mb_restore(r, text) : strdup("");
    cJSON_Delete(resp);
  }

  cJSON_Delete(messages);
  cJSON_Delete(tools);
  mb_redactor_free(r);

  if (e != MB_OK) { free(reply); return e; }
  if (!reply) return MB_FAIL(MB_ERR_INTERNAL, "agent did not produce a reply (tool loop exhausted)");
  *reply_out = reply;
  return MB_OK;
}

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../account/account.h"
#include "../counterparty/counterparty.h"
#include "../journal/journal.h"

/* mock provider: returns canned responses in sequence; captures the last request */
struct mock {
  const char **responses;
  int          count;
  int          step;
  char         last_request[2048];
};
static char *mock_complete(void *ctx, const char *request_json) {
  struct mock *m = ctx;
  snprintf(m->last_request, sizeof m->last_request, "%s", request_json);
  if (m->step >= m->count) return NULL;
  return strdup(m->responses[m->step++]);
}

TEST(agent, tool_loop_executes_and_replies) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40];
  mb_account_new b = {.name="Bank", .type=MB_ACCT_ASSET, .role=MB_ROLE_ACCOUNT};
  mb_account_new i = {.name="Consulting Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &b, bank));
  ASSERT_OK(mb_account_create(s, &i, income));

  char tool_resp[512];
  snprintf(tool_resp, sizeof tool_resp,
    "{\"tool_calls\":[{\"id\":\"t1\",\"name\":\"income.record\",\"arguments\":"
    "{\"date\":\"2026-09-01\",\"amount\":50000,\"deposit_account_id\":\"%s\",\"category_id\":\"%s\"}}]}",
    bank, income);
  const char *final_resp = "{\"text\":\"Done — recorded your $500 income.\"}";
  const char *seq[] = { tool_resp, final_resp };
  struct mock m = {.responses = seq, .count = 2, .step = 0};
  mb_llm_provider prov = {.complete = mock_complete, .ctx = &m};

  char *reply = NULL;
  ASSERT_OK(mb_agent_run(s, &prov, "log my 500 consulting income", &reply));
  ASSERT_STR_EQ(reply, "Done — recorded your $500 income.");
  free(reply);

  /* the tool actually posted to the books */
  mb_money bal;
  ASSERT_OK(mb_account_balance(s, income, &bal));
  ASSERT_MONEY_EQ(bal, -50000);
  mb_store_close(s);
}

TEST(agent, redacts_before_sending) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char cp[40];
  mb_counterparty_new c = {.name = "Acme Corporation", .kind = MB_CP_CUSTOMER};
  ASSERT_OK(mb_counterparty_create(s, &c, cp));

  const char *seq[] = { "{\"text\":\"ok\"}" };
  struct mock m = {.responses = seq, .count = 1, .step = 0};
  mb_llm_provider prov = {.complete = mock_complete, .ctx = &m};

  char *reply = NULL;
  ASSERT_OK(mb_agent_run(s, &prov, "How much does Acme Corporation owe me?", &reply));
  free(reply);
  /* the real name never reached the provider; a token did */
  ASSERT(strstr(m.last_request, "Acme Corporation") == NULL);
  ASSERT(strstr(m.last_request, "Client_1") != NULL);
  mb_store_close(s);
}
#endif
