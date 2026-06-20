#include "mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vendor/cjson/cJSON.h"
#include "../api/api.h"

#define MCP_PROTOCOL "2025-11-25"

/* ---- tool registry: MCP tool -> api method ---- */
typedef struct {
  const char *name;
  const char *api_method;
  int         is_write;
  const char *description;
  const char *schema;   /* JSON Schema for inputSchema */
} mcp_tool;

#define OBJ(props, req) "{\"type\":\"object\",\"properties\":" props ",\"required\":" req "}"

static const mcp_tool TOOLS[] = {
  {"list_accounts", "account.list", 0, "List accounts and categories (optionally filtered).",
   OBJ("{\"type\":{\"type\":\"string\"},\"role\":{\"type\":\"string\"},\"active_only\":{\"type\":\"boolean\"}}", "[]")},
  {"get_account", "account.get", 0, "Get one account by id.",
   OBJ("{\"id\":{\"type\":\"string\"}}", "[\"id\"]")},
  {"create_account", "account.create", 1, "Create an account or category.",
   OBJ("{\"name\":{\"type\":\"string\"},\"type\":{\"type\":\"string\",\"enum\":[\"ASSET\",\"LIABILITY\",\"EQUITY\",\"INCOME\",\"EXPENSE\"]},\"role\":{\"type\":\"string\",\"enum\":[\"ACCOUNT\",\"CATEGORY\"]},\"code\":{\"type\":\"string\"}}", "[\"name\",\"type\",\"role\"]")},
  {"record_income", "income.record", 1, "Record income: debit a deposit account, credit an income category.",
   OBJ("{\"date\":{\"type\":\"string\"},\"amount\":{\"type\":\"integer\",\"description\":\"minor units (cents)\"},\"deposit_account_id\":{\"type\":\"string\"},\"category_id\":{\"type\":\"string\"},\"memo\":{\"type\":\"string\"}}", "[\"date\",\"amount\",\"deposit_account_id\",\"category_id\"]")},
  {"record_expense", "expense.record", 1, "Record an expense: debit an expense category, credit a payment account.",
   OBJ("{\"date\":{\"type\":\"string\"},\"amount\":{\"type\":\"integer\",\"description\":\"cents\"},\"pay_account_id\":{\"type\":\"string\"},\"category_id\":{\"type\":\"string\"},\"memo\":{\"type\":\"string\"}}", "[\"date\",\"amount\",\"pay_account_id\",\"category_id\"]")},
  {"post_transaction", "transaction.post", 1, "Post a balanced journal entry (postings sum to zero; + debit, - credit).",
   OBJ("{\"date\":{\"type\":\"string\"},\"memo\":{\"type\":\"string\"},\"postings\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"account_id\":{\"type\":\"string\"},\"amount\":{\"type\":\"integer\"},\"memo\":{\"type\":\"string\"}}}}}", "[\"date\",\"postings\"]")},
  {"list_counterparties", "counterparty.list", 0, "List clients/vendors.",
   OBJ("{\"active_only\":{\"type\":\"boolean\"}}", "[]")},
  {"create_counterparty", "counterparty.create", 1, "Create a client or vendor.",
   OBJ("{\"name\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\",\"enum\":[\"CUSTOMER\",\"VENDOR\",\"BOTH\"]},\"email\":{\"type\":\"string\"}}", "[\"name\"]")},
  {"create_invoice", "invoice.create", 1, "Create a draft invoice for a counterparty.",
   OBJ("{\"counterparty_id\":{\"type\":\"string\"},\"number\":{\"type\":\"string\"},\"due_date\":{\"type\":\"string\"},\"memo\":{\"type\":\"string\"}}", "[\"counterparty_id\"]")},
  {"add_invoice_line", "invoice.add_line", 1, "Add a line to a draft invoice (tax line -> set is_tax + a tax-payable account).",
   OBJ("{\"invoice_id\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},\"qty_centi\":{\"type\":\"integer\"},\"unit_price\":{\"type\":\"integer\"},\"account_id\":{\"type\":\"string\"},\"is_tax\":{\"type\":\"boolean\"}}", "[\"invoice_id\",\"description\",\"unit_price\",\"account_id\"]")},
  {"issue_invoice", "invoice.issue", 1, "Issue a draft invoice (posts Dr AR / Cr income).",
   OBJ("{\"invoice_id\":{\"type\":\"string\"},\"issue_date\":{\"type\":\"string\"}}", "[\"invoice_id\",\"issue_date\"]")},
  {"get_invoice", "invoice.get", 0, "Get an invoice with its total and status.",
   OBJ("{\"id\":{\"type\":\"string\"}}", "[\"id\"]")},
  {"list_invoices", "invoice.list", 0, "List all invoices (id, number, counterparty, issue/due date, status, total). Filter or count by status client-side (DRAFT/OPEN/PARTIAL/PAID/VOID).",
   OBJ("{}", "[]")},
  {"create_bill", "bill.create", 1, "Create a draft bill from a vendor.",
   OBJ("{\"counterparty_id\":{\"type\":\"string\"},\"number\":{\"type\":\"string\"},\"due_date\":{\"type\":\"string\"},\"memo\":{\"type\":\"string\"}}", "[\"counterparty_id\"]")},
  {"add_bill_line", "bill.add_line", 1, "Add a line to a draft bill.",
   OBJ("{\"bill_id\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},\"qty_centi\":{\"type\":\"integer\"},\"unit_price\":{\"type\":\"integer\"},\"account_id\":{\"type\":\"string\"},\"is_tax\":{\"type\":\"boolean\"}}", "[\"bill_id\",\"description\",\"unit_price\",\"account_id\"]")},
  {"enter_bill", "bill.enter", 1, "Enter a draft bill (posts Dr expense / Cr AP).",
   OBJ("{\"bill_id\":{\"type\":\"string\"},\"issue_date\":{\"type\":\"string\"}}", "[\"bill_id\",\"issue_date\"]")},
  {"get_bill", "bill.get", 0, "Get a bill with its total and status.",
   OBJ("{\"id\":{\"type\":\"string\"}}", "[\"id\"]")},
  {"list_bills", "bill.list", 0, "List all bills (id, number, vendor, issue/due date, status, total). Filter or count by status client-side (DRAFT/OPEN/PARTIAL/PAID/VOID).",
   OBJ("{}", "[]")},
  {"record_payment", "payment.record", 1, "Record a payment against an invoice or bill.",
   OBJ("{\"date\":{\"type\":\"string\"},\"amount\":{\"type\":\"integer\"},\"cash_account_id\":{\"type\":\"string\"},\"target\":{\"type\":\"string\",\"enum\":[\"INVOICE\",\"BILL\"]},\"target_id\":{\"type\":\"string\"}}", "[\"date\",\"amount\",\"cash_account_id\",\"target\",\"target_id\"]")},
  {"report_trial_balance", "report.trial_balance", 0, "Trial balance as of a date (debits must equal credits).",
   OBJ("{\"as_of\":{\"type\":\"string\"}}", "[]")},
  {"report_pnl", "report.pnl", 0, "Profit & loss over a date range.",
   OBJ("{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}}", "[]")},
  {"report_balance_sheet", "report.balance_sheet", 0, "Balance sheet as of a date.",
   OBJ("{\"as_of\":{\"type\":\"string\"}}", "[]")},
  {"report_cash_flow", "report.cash_flow", 0, "Cash flow over a date range.",
   OBJ("{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}}", "[]")},
};
static const int TOOL_COUNT = (int)(sizeof TOOLS / sizeof TOOLS[0]);

/* ---- permission policy (D18) ---- */
static void tool_policy(mb_store *s, const mcp_tool *t, char out[8]) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT policy FROM tool_permission WHERE tool=?;",
                         -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, t->name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
      snprintf(out, 8, "%s", (const char *)sqlite3_column_text(st, 0));
      sqlite3_finalize(st);
      return;
    }
    sqlite3_finalize(st);
  }
  snprintf(out, 8, "%s", t->is_write ? "ASK" : "PERMIT");  /* default */
}

mb_err mb_mcp_set_policy(mb_store *s, const char *tool, const char *policy) {
  if (strcmp(policy, "PERMIT") && strcmp(policy, "ASK") && strcmp(policy, "BLOCK"))
    return MB_FAIL(MB_ERR_INVALID_ARG, "policy must be PERMIT/ASK/BLOCK");
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO tool_permission(tool,policy) VALUES(?,?) "
        "ON CONFLICT(tool) DO UPDATE SET policy=excluded.policy;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, tool, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, policy, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

mb_err mb_mcp_tools_catalog(mb_store *s, char **json_out) {
  cJSON *r = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(r, "tools");
  for (int i = 0; i < TOOL_COUNT; i++) {
    char policy[8]; tool_policy(s, &TOOLS[i], policy);
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", TOOLS[i].name);
    cJSON_AddStringToObject(t, "description", TOOLS[i].description);
    cJSON_AddBoolToObject(t, "is_write", TOOLS[i].is_write);
    cJSON_AddStringToObject(t, "policy", policy);
    cJSON_AddItemToArray(arr, t);
  }
  cJSON_AddNumberToObject(r, "count", TOOL_COUNT);
  *json_out = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return *json_out ? MB_OK : MB_FAIL(MB_ERR_INTERNAL, "catalog print failed");
}

/* ---- method handlers (return a cJSON result object, or NULL) ---- */
static cJSON *do_initialize(void) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "protocolVersion", MCP_PROTOCOL);
  cJSON *caps = cJSON_AddObjectToObject(r, "capabilities");
  cJSON *tools = cJSON_AddObjectToObject(caps, "tools");
  cJSON_AddBoolToObject(tools, "listChanged", 0);
  cJSON *info = cJSON_AddObjectToObject(r, "serverInfo");
  cJSON_AddStringToObject(info, "name", "money-books");
  cJSON_AddStringToObject(info, "title", "Money Books");
  cJSON_AddStringToObject(info, "version", "0.1.0");
  return r;
}

static cJSON *do_tools_list(mb_store *s) {
  cJSON *r = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(r, "tools");
  for (int i = 0; i < TOOL_COUNT; i++) {
    char policy[8];
    tool_policy(s, &TOOLS[i], policy);
    if (!strcmp(policy, "BLOCK")) continue;   /* hidden */
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", TOOLS[i].name);
    cJSON *schema = cJSON_Parse(TOOLS[i].schema);
    if (TOOLS[i].is_write) {
      /* surface the approval contract in the schema + description so clients/agents see it */
      char desc[512];
      snprintf(desc, sizeof desc, "%s  [WRITE — requires user approval: the first call returns an "
               "approval request and writes nothing; re-call with confirm=true to execute.]",
               TOOLS[i].description);
      cJSON_AddStringToObject(t, "description", desc);
      cJSON *props = cJSON_GetObjectItem(schema, "properties");
      if (props) {
        cJSON *cf = cJSON_AddObjectToObject(props, "confirm");
        cJSON_AddStringToObject(cf, "type", "boolean");
        cJSON_AddStringToObject(cf, "description",
          "Set true to execute. Omit/false → server returns an approval request and writes nothing.");
      }
    } else {
      cJSON_AddStringToObject(t, "description", TOOLS[i].description);
    }
    cJSON_AddItemToObject(t, "inputSchema", schema);
    cJSON_AddItemToArray(arr, t);
  }
  return r;
}

/* MCP CallToolResult: {content:[{type:text,text}], isError?, structuredContent?} */
static cJSON *tool_error(const char *msg) {
  cJSON *r = cJSON_CreateObject();
  cJSON *c = cJSON_AddArrayToObject(r, "content");
  cJSON *t = cJSON_CreateObject();
  cJSON_AddStringToObject(t, "type", "text");
  cJSON_AddStringToObject(t, "text", msg);
  cJSON_AddItemToArray(c, t);
  cJSON_AddBoolToObject(r, "isError", 1);
  return r;
}

/* A write tool was called without confirm=true: return a human-facing approval request and do
 * NOT execute. The client/agent must show this to the user and re-call with confirm=true. This is
 * a server-side gate, stronger than the PERMIT/ASK policy (which delegates "ask" to the client). */
static cJSON *approval_request(const mcp_tool *t, const cJSON *args) {
  char *argstr = args ? cJSON_PrintUnformatted(args) : NULL;
  size_t n = (argstr ? strlen(argstr) : 2) + 512;
  char *msg = malloc(n);
  if (msg)
    snprintf(msg, n,
      "APPROVAL REQUIRED — this writes to the books and must be confirmed by the user, regardless "
      "of permission settings. Nothing has been written yet.\n\n"
      "Tool: %s — %s\nArguments: %s\n\n"
      "Show this to the user, get explicit approval, then call \"%s\" again with \"confirm\": true.",
      t->name, t->description, argstr ? argstr : "{}", t->name);
  cJSON *r = cJSON_CreateObject();
  cJSON *c = cJSON_AddArrayToObject(r, "content");
  cJSON *txt = cJSON_CreateObject();
  cJSON_AddStringToObject(txt, "type", "text");
  cJSON_AddStringToObject(txt, "text", msg ? msg : "APPROVAL REQUIRED — re-call with confirm=true.");
  cJSON_AddItemToArray(c, txt);
  cJSON *sc = cJSON_AddObjectToObject(r, "structuredContent");
  cJSON_AddBoolToObject(sc, "approval_required", 1);
  cJSON_AddStringToObject(sc, "tool", t->name);
  free(msg);
  free(argstr);
  return r;
}

static cJSON *do_tools_call(mb_store *s, const cJSON *params) {
  const cJSON *nameItem = cJSON_GetObjectItemCaseSensitive(params, "name");
  if (!cJSON_IsString(nameItem)) return tool_error("missing tool name");
  const char *name = nameItem->valuestring;

  const mcp_tool *tool = NULL;
  for (int i = 0; i < TOOL_COUNT; i++)
    if (!strcmp(TOOLS[i].name, name)) { tool = &TOOLS[i]; break; }
  if (!tool) return tool_error("unknown tool");

  char policy[8];
  tool_policy(s, tool, policy);
  if (!strcmp(policy, "BLOCK")) return tool_error("tool is blocked by permission policy");

  const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");

  /* Mutation gate: every write requires explicit user approval, regardless of PERMIT/ASK policy.
   * Without confirm=true the server returns an approval request and changes nothing; the user
   * approves, then the tool is re-called with confirm=true. (BLOCK above still hard-refuses.) */
  if (tool->is_write) {
    const cJSON *confirm = args ? cJSON_GetObjectItemCaseSensitive(args, "confirm") : NULL;
    if (!cJSON_IsTrue(confirm)) return approval_request(tool, args);
  }

  char *args_str = args ? cJSON_PrintUnformatted(args) : NULL;

  char *result_json = NULL;
  mb_err e = mb_api_dispatch(s, tool->api_method, args_str ? args_str : "{}", &result_json);
  free(args_str);

  cJSON *r = cJSON_CreateObject();
  cJSON *c = cJSON_AddArrayToObject(r, "content");
  cJSON *t = cJSON_CreateObject();
  cJSON_AddStringToObject(t, "type", "text");
  cJSON_AddStringToObject(t, "text", result_json ? result_json : "{}");
  cJSON_AddItemToArray(c, t);
  if (result_json) {
    cJSON *parsed = cJSON_Parse(result_json);
    if (parsed) cJSON_AddItemToObject(r, "structuredContent", parsed);
  }
  if (e != MB_OK) cJSON_AddBoolToObject(r, "isError", 1);
  free(result_json);
  return r;
}

mb_err mb_mcp_handle(mb_store *s, const char *msg_json, char **out_json) {
  *out_json = NULL;
  cJSON *req = cJSON_Parse(msg_json);
  if (!req) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddNullToObject(resp, "id");
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code", -32700);
    cJSON_AddStringToObject(err, "message", "parse error");
    *out_json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return MB_OK;
  }

  const cJSON *idItem = cJSON_GetObjectItem(req, "id");
  const cJSON *methodItem = cJSON_GetObjectItem(req, "method");
  const char *method = cJSON_IsString(methodItem) ? methodItem->valuestring : NULL;
  cJSON *params = cJSON_GetObjectItem(req, "params");
  int is_notification = (idItem == NULL);

  cJSON *result = NULL;
  int err_code = 0;
  const char *err_msg = NULL;

  if (!method) {
    err_code = -32600; err_msg = "invalid request";
  } else if (!strcmp(method, "initialize")) {
    result = do_initialize();
  } else if (!strcmp(method, "notifications/initialized")) {
    /* notification, no result */
  } else if (!strcmp(method, "ping")) {
    result = cJSON_CreateObject();
  } else if (!strcmp(method, "tools/list")) {
    result = do_tools_list(s);
  } else if (!strcmp(method, "tools/call")) {
    result = do_tools_call(s, params);
  } else {
    err_code = -32601; err_msg = "method not found";
  }

  if (is_notification) {            /* no reply for notifications */
    if (result) cJSON_Delete(result);
    cJSON_Delete(req);
    return MB_OK;
  }

  cJSON *resp = cJSON_CreateObject();
  cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
  cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(idItem, 1));
  if (err_code) {
    if (result) cJSON_Delete(result);
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code", err_code);
    cJSON_AddStringToObject(err, "message", err_msg);
  } else {
    cJSON_AddItemToObject(resp, "result", result ? result : cJSON_CreateObject());
  }
  *out_json = cJSON_PrintUnformatted(resp);
  cJSON_Delete(resp);
  cJSON_Delete(req);
  return MB_OK;
}

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../account/account.h"
#include "../seed/seed.h"

static cJSON *call(mb_store *s, const char *msg) {
  char *out = NULL;
  (void)mb_mcp_handle(s, msg, &out);
  cJSON *j = out ? cJSON_Parse(out) : NULL;
  free(out);
  return j;
}

TEST(mcp, initialize) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  cJSON *j = call(s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
  cJSON *res = cJSON_GetObjectItem(j, "result");
  ASSERT_STR_EQ(cJSON_GetObjectItem(res, "protocolVersion")->valuestring, MCP_PROTOCOL);
  cJSON_Delete(j);
  mb_store_close(s);
}

TEST(mcp, tools_list_has_tools) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  cJSON *j = call(s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
  cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "tools");
  ASSERT(cJSON_GetArraySize(tools) >= 15);
  int found = 0;
  cJSON *it;
  cJSON_ArrayForEach(it, tools)
    if (!strcmp(cJSON_GetObjectItem(it, "name")->valuestring, "record_income")) found = 1;
  ASSERT_EQ_INT(found, 1);
  cJSON_Delete(j);
  mb_store_close(s);
}

TEST(mcp, tools_catalog) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char *json = NULL;
  ASSERT_OK(mb_mcp_tools_catalog(s, &json));
  cJSON *j = cJSON_Parse(json); free(json);
  ASSERT(j != NULL);
  cJSON *tools = cJSON_GetObjectItem(j, "tools");
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(j, "count")->valuedouble, cJSON_GetArraySize(tools));
  ASSERT(cJSON_GetArraySize(tools) >= 20);
  int checked = 0; cJSON *it;
  cJSON_ArrayForEach(it, tools)
    if (!strcmp(cJSON_GetObjectItem(it, "name")->valuestring, "record_income")) {
      ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(it, "is_write")));
      ASSERT(cJSON_GetObjectItem(it, "policy") != NULL);
      checked = 1;
    }
  ASSERT_EQ_INT(checked, 1);
  cJSON_Delete(j);
  mb_store_close(s);
}

TEST(mcp, tools_call_records_income) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40];
  mb_account_new b = {.name="Bank", .type=MB_ACCT_ASSET, .role=MB_ROLE_ACCOUNT};
  mb_account_new i = {.name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &b, bank));
  ASSERT_OK(mb_account_create(s, &i, income));

  char msg[400];
  snprintf(msg, sizeof msg,
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"record_income\","
    "\"arguments\":{\"date\":\"2026-09-01\",\"amount\":50000,\"deposit_account_id\":\"%s\",\"category_id\":\"%s\",\"confirm\":true}}}",
    bank, income);
  cJSON *j = call(s, msg);
  cJSON *res = cJSON_GetObjectItem(j, "result");
  ASSERT(cJSON_GetObjectItem(res, "isError") == NULL);   /* success */
  cJSON_Delete(j);

  /* verify via report_pnl tool */
  j = call(s, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"report_pnl\",\"arguments\":{}}}");
  cJSON *sc = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "structuredContent");
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(sc, "income")->valuedouble, 50000);
  cJSON_Delete(j);
  mb_store_close(s);
}

TEST(mcp, ping_and_unknown_method) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  cJSON *j = call(s, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\"}");
  ASSERT(cJSON_GetObjectItem(j, "result") != NULL);
  cJSON_Delete(j);
  j = call(s, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"bogus/method\"}");
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(cJSON_GetObjectItem(j, "error"), "code")->valuedouble, -32601);
  cJSON_Delete(j);
  mb_store_close(s);
}

TEST(mcp, blocked_tool_hidden_and_refused) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_mcp_set_policy(s, "record_expense", "BLOCK"));
  /* hidden from list */
  cJSON *j = call(s, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}");
  cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "tools");
  cJSON *it; int found = 0;
  cJSON_ArrayForEach(it, tools)
    if (!strcmp(cJSON_GetObjectItem(it, "name")->valuestring, "record_expense")) found = 1;
  ASSERT_EQ_INT(found, 0);
  cJSON_Delete(j);
  /* refused on call */
  j = call(s, "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"record_expense\",\"arguments\":{}}}");
  cJSON *res = cJSON_GetObjectItem(j, "result");
  ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "isError")));
  cJSON_Delete(j);
  mb_store_close(s);
}

TEST(mcp, write_requires_confirmation) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40];
  mb_account_new b = {.name="Bank", .type=MB_ACCT_ASSET, .role=MB_ROLE_ACCOUNT};
  mb_account_new i = {.name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &b, bank));
  ASSERT_OK(mb_account_create(s, &i, income));

  char base[300];
  snprintf(base, sizeof base,
    "\"date\":\"2026-09-01\",\"amount\":50000,\"deposit_account_id\":\"%s\",\"category_id\":\"%s\"", bank, income);

  /* 1) no confirm → approval request, nothing posted */
  char msg[500];
  snprintf(msg, sizeof msg,
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"record_income\",\"arguments\":{%s}}}", base);
  cJSON *j = call(s, msg);
  cJSON *sc = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "structuredContent");
  ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(sc, "approval_required")));
  cJSON_Delete(j);
  j = call(s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"report_pnl\",\"arguments\":{}}}");
  sc = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "structuredContent");
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(sc, "income")->valuedouble, 0);   /* unchanged */
  cJSON_Delete(j);

  /* 2) confirm:true → executes */
  snprintf(msg, sizeof msg,
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"record_income\",\"arguments\":{%s,\"confirm\":true}}}", base);
  j = call(s, msg);
  ASSERT(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "isError") == NULL);
  cJSON_Delete(j);
  j = call(s, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"report_pnl\",\"arguments\":{}}}");
  sc = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "structuredContent");
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(sc, "income")->valuedouble, 50000);   /* posted */
  cJSON_Delete(j);
  mb_store_close(s);
}

TEST(mcp, notification_no_reply) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char *out = (char *)1;
  ASSERT_OK(mb_mcp_handle(s, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", &out));
  ASSERT(out == NULL);   /* no response for a notification */
  mb_store_close(s);
}
#endif
