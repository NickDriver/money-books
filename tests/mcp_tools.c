/*
 * Integration test: drive EVERY MCP tool through the JSON-RPC layer (mb_mcp_handle), the same
 * path an external client (Claude Desktop) uses. One end-to-end session touches all 23 tools and
 * verifies the write-approval gate. New tools added to TOOLS[] must be covered here too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/mcp/mcp.h"
#include "../src/store/store.h"
#include "../src/seed/seed.h"
#include "../src/account/account.h"
#include "../src/support/mb_test.h"
#include "../src/vendor/cjson/cJSON.h"

/* ---- helpers (no ASSERT here — assertions live in the test body where `t` is in scope) ---- */
static cJSON *call(mb_store *s, const char *msg) {
  char *out = NULL;
  (void)mb_mcp_handle(s, msg, &out);
  cJSON *j = out ? cJSON_Parse(out) : NULL;
  free(out);
  return j;
}
static cJSON *tcall(mb_store *s, const char *name, const char *args) {
  char msg[700];
  snprintf(msg, sizeof msg,
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
    name, (args && args[0]) ? args : "{}");
  return call(s, msg);
}
static cJSON *sc(cJSON *resp) {   /* the tool's structuredContent (parsed api result) */
  return cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "structuredContent");
}
static int tool_ok(cJSON *resp) {  /* result present and not flagged isError */
  cJSON *res = cJSON_GetObjectItem(resp, "result");
  return res && cJSON_GetObjectItem(res, "isError") == NULL;
}
static void grab(cJSON *resp, const char *key, char *out, size_t n) {
  cJSON *scc = sc(resp);
  const char *v = scc ? cJSON_GetStringValue(cJSON_GetObjectItem(scc, key)) : NULL;
  snprintf(out, n, "%s", v ? v : "");
}
static long scnum(cJSON *resp, const char *key) {
  cJSON *scc = sc(resp);
  cJSON *v = scc ? cJSON_GetObjectItem(scc, key) : NULL;
  return v ? (long)v->valuedouble : -1;
}
static const char *scstr(cJSON *resp, const char *key) {
  cJSON *scc = sc(resp);
  const char *v = scc ? cJSON_GetStringValue(cJSON_GetObjectItem(scc, key)) : NULL;
  return v ? v : "";
}

TEST(mcp_tools, every_tool_end_to_end) {
  mb_store *s = NULL;
  ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_starter_chart(s));   /* full freelancer chart: AR/AP/tax + income/expense */

  /* MCP handshake */
  cJSON *j = call(s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
  ASSERT_STR_EQ(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "protocolVersion")->valuestring, "2025-11-25");
  cJSON_Delete(j);
  (void)call(s, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

  /* resolve the account ids we need (by chart code) */
  mb_account a;
  char checking[40], savings[40], income[40], expense[40];
  ASSERT_OK(mb_account_find_by_code(s, "1000", &a)); snprintf(checking, 40, "%s", a.id);
  ASSERT_OK(mb_account_find_by_code(s, "1010", &a)); snprintf(savings,  40, "%s", a.id);
  ASSERT_OK(mb_account_find_by_code(s, "4000", &a)); snprintf(income,   40, "%s", a.id);
  ASSERT_OK(mb_account_find_by_code(s, "6010", &a)); snprintf(expense,  40, "%s", a.id);

  char args[700];

  /* ===== READ tools (baseline) ===== */
  j = tcall(s, "list_accounts", "{}");
  ASSERT_TRUE(tool_ok(j));
  ASSERT(cJSON_GetArraySize(cJSON_GetObjectItem(sc(j), "accounts")) > 0);
  cJSON_Delete(j);

  snprintf(args, sizeof args, "{\"id\":\"%s\"}", checking);
  j = tcall(s, "get_account", args);
  ASSERT_TRUE(tool_ok(j));
  ASSERT_STR_EQ(cJSON_GetObjectItem(cJSON_GetObjectItem(sc(j), "account"), "code")->valuestring, "1000");
  cJSON_Delete(j);

  j = tcall(s, "report_trial_balance", "{}");
  ASSERT_TRUE(tool_ok(j) && cJSON_IsTrue(cJSON_GetObjectItem(sc(j), "balanced")));
  cJSON_Delete(j);
  j = tcall(s, "report_pnl", "{}");
  ASSERT_TRUE(tool_ok(j)); ASSERT_EQ_INT(scnum(j, "income"), 0);
  cJSON_Delete(j);
  j = tcall(s, "report_balance_sheet", "{}");
  ASSERT_TRUE(tool_ok(j) && cJSON_IsTrue(cJSON_GetObjectItem(sc(j), "balanced")));
  cJSON_Delete(j);
  j = tcall(s, "report_cash_flow", "{}");
  ASSERT_TRUE(tool_ok(j)); ASSERT_EQ_INT(scnum(j, "net"), 0);
  cJSON_Delete(j);

  /* ===== approval gate: a write without confirm changes nothing ===== */
  snprintf(args, sizeof args, "{\"date\":\"2026-06-19\",\"amount\":25000,\"deposit_account_id\":\"%s\",\"category_id\":\"%s\"}", checking, income);
  j = tcall(s, "record_income", args);
  ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(sc(j), "approval_required")));
  cJSON_Delete(j);
  j = tcall(s, "report_pnl", "{}");
  ASSERT_EQ_INT(scnum(j, "income"), 0);   /* still nothing posted */
  cJSON_Delete(j);

  /* ===== WRITE tools (all confirmed) ===== */
  j = tcall(s, "create_account", "{\"name\":\"MCP Test Cat\",\"type\":\"EXPENSE\",\"role\":\"CATEGORY\",\"code\":\"6999\",\"confirm\":true}");
  ASSERT_TRUE(tool_ok(j));
  cJSON_Delete(j);

  char cust[40], vend[40];
  j = tcall(s, "create_counterparty", "{\"name\":\"MCP Client\",\"kind\":\"CUSTOMER\",\"confirm\":true}");
  ASSERT_TRUE(tool_ok(j)); grab(j, "id", cust, 40); cJSON_Delete(j);
  j = tcall(s, "create_counterparty", "{\"name\":\"MCP Vendor\",\"kind\":\"VENDOR\",\"confirm\":true}");
  ASSERT_TRUE(tool_ok(j)); grab(j, "id", vend, 40); cJSON_Delete(j);

  j = tcall(s, "list_counterparties", "{}");
  ASSERT_TRUE(tool_ok(j));
  ASSERT(cJSON_GetArraySize(cJSON_GetObjectItem(sc(j), "counterparties")) >= 2);
  cJSON_Delete(j);

  snprintf(args, sizeof args, "{\"date\":\"2026-06-19\",\"amount\":25000,\"deposit_account_id\":\"%s\",\"category_id\":\"%s\",\"confirm\":true}", checking, income);
  j = tcall(s, "record_income", args); ASSERT_TRUE(tool_ok(j)); cJSON_Delete(j);

  snprintf(args, sizeof args, "{\"date\":\"2026-06-19\",\"amount\":5000,\"pay_account_id\":\"%s\",\"category_id\":\"%s\",\"confirm\":true}", checking, expense);
  j = tcall(s, "record_expense", args); ASSERT_TRUE(tool_ok(j)); cJSON_Delete(j);

  snprintf(args, sizeof args,
    "{\"date\":\"2026-06-19\",\"memo\":\"transfer\",\"postings\":[{\"account_id\":\"%s\",\"amount\":10000},{\"account_id\":\"%s\",\"amount\":-10000}],\"confirm\":true}",
    savings, checking);
  j = tcall(s, "post_transaction", args); ASSERT_TRUE(tool_ok(j)); cJSON_Delete(j);

  /* invoice flow */
  char inv[40];
  snprintf(args, sizeof args, "{\"counterparty_id\":\"%s\",\"number\":\"MCP-INV-1\",\"confirm\":true}", cust);
  j = tcall(s, "create_invoice", args); ASSERT_TRUE(tool_ok(j)); grab(j, "id", inv, 40); cJSON_Delete(j);

  snprintf(args, sizeof args,
    "{\"invoice_id\":\"%s\",\"description\":\"Consulting\",\"unit_price\":30000,\"qty_centi\":100,\"account_id\":\"%s\",\"confirm\":true}", inv, income);
  j = tcall(s, "add_invoice_line", args); ASSERT_TRUE(tool_ok(j)); cJSON_Delete(j);

  snprintf(args, sizeof args, "{\"invoice_id\":\"%s\",\"issue_date\":\"2026-06-19\",\"confirm\":true}", inv);
  j = tcall(s, "issue_invoice", args); ASSERT_TRUE(tool_ok(j)); cJSON_Delete(j);

  snprintf(args, sizeof args, "{\"id\":\"%s\"}", inv);
  j = tcall(s, "get_invoice", args);
  ASSERT_TRUE(tool_ok(j));
  ASSERT_STR_EQ(scstr(j, "status"), "OPEN");
  ASSERT_EQ_INT(scnum(j, "total"), 30000);
  cJSON_Delete(j);

  j = tcall(s, "list_invoices", "{}");
  ASSERT_TRUE(tool_ok(j));
  ASSERT(cJSON_GetArraySize(cJSON_GetObjectItem(sc(j), "invoices")) >= 1);
  cJSON_Delete(j);

  snprintf(args, sizeof args, "{\"date\":\"2026-06-19\",\"amount\":30000,\"cash_account_id\":\"%s\",\"target\":\"INVOICE\",\"target_id\":\"%s\",\"confirm\":true}", checking, inv);
  j = tcall(s, "record_payment", args); ASSERT_TRUE(tool_ok(j)); cJSON_Delete(j);
  snprintf(args, sizeof args, "{\"id\":\"%s\"}", inv);
  j = tcall(s, "get_invoice", args); ASSERT_STR_EQ(scstr(j, "status"), "PAID"); cJSON_Delete(j);

  /* bill flow */
  char bill[40];
  snprintf(args, sizeof args, "{\"counterparty_id\":\"%s\",\"number\":\"MCP-BILL-1\",\"confirm\":true}", vend);
  j = tcall(s, "create_bill", args); ASSERT_TRUE(tool_ok(j)); grab(j, "id", bill, 40); cJSON_Delete(j);

  snprintf(args, sizeof args,
    "{\"bill_id\":\"%s\",\"description\":\"Hosting\",\"unit_price\":8000,\"qty_centi\":100,\"account_id\":\"%s\",\"confirm\":true}", bill, expense);
  j = tcall(s, "add_bill_line", args); ASSERT_TRUE(tool_ok(j)); cJSON_Delete(j);

  snprintf(args, sizeof args, "{\"bill_id\":\"%s\",\"issue_date\":\"2026-06-19\",\"confirm\":true}", bill);
  j = tcall(s, "enter_bill", args); ASSERT_TRUE(tool_ok(j)); cJSON_Delete(j);

  snprintf(args, sizeof args, "{\"id\":\"%s\"}", bill);
  j = tcall(s, "get_bill", args);
  ASSERT_TRUE(tool_ok(j));
  ASSERT_STR_EQ(scstr(j, "status"), "OPEN");
  cJSON_Delete(j);

  j = tcall(s, "list_bills", "{}");
  ASSERT_TRUE(tool_ok(j));
  ASSERT(cJSON_GetArraySize(cJSON_GetObjectItem(sc(j), "bills")) >= 1);
  cJSON_Delete(j);

  snprintf(args, sizeof args, "{\"date\":\"2026-06-19\",\"amount\":8000,\"cash_account_id\":\"%s\",\"target\":\"BILL\",\"target_id\":\"%s\",\"confirm\":true}", checking, bill);
  j = tcall(s, "record_payment", args); ASSERT_TRUE(tool_ok(j)); cJSON_Delete(j);
  snprintf(args, sizeof args, "{\"id\":\"%s\"}", bill);
  j = tcall(s, "get_bill", args); ASSERT_STR_EQ(scstr(j, "status"), "PAID"); cJSON_Delete(j);

  /* ===== final reconciliation: income 25000+30000, expense 5000+8000, books balanced ===== */
  j = tcall(s, "report_pnl", "{}");
  ASSERT_EQ_INT(scnum(j, "income"), 55000);
  ASSERT_EQ_INT(scnum(j, "expense"), 13000);
  ASSERT_EQ_INT(scnum(j, "net"), 42000);
  cJSON_Delete(j);
  j = tcall(s, "report_trial_balance", "{}");
  ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(sc(j), "balanced")));
  cJSON_Delete(j);
  j = tcall(s, "report_balance_sheet", "{}");
  ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(sc(j), "balanced")));
  cJSON_Delete(j);

  mb_store_close(s);
}
