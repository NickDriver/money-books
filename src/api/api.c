#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vendor/cjson/cJSON.h"
#include "../account/account.h"
#include "../item/item.h"
#include "../report/report.h"
#include "../export/export.h"
#include "../journal/journal.h"
#include "../counterparty/counterparty.h"
#include "../invoice/invoice.h"
#include "../bill/bill.h"
#include "../payment/payment.h"
#include "../seed/seed.h"
#include "../book/book.h"
#include "../mcp/mcp.h"

/* ---- small helpers ---- */
static const char *jstr(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}
static int jbool(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsTrue(v) ? 1 : 0;
}
static long long jint(const cJSON *o, const char *key, long long def) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsNumber(v) ? (long long)v->valuedouble : def;
}

/* app_setting helpers are defined lower down (with the LLM settings); forward-declare them
 * so the onboarding handlers above can read/write the "onboarded" flag. */
static mb_err app_setting_get(mb_store *s, const char *k, char *buf, size_t n);
static mb_err app_setting_set(mb_store *s, const char *k, const char *v);
static mb_source parse_source(const char *s) {
  if (s && !strcmp(s, "AI")) return MB_SRC_AI;
  if (s && !strcmp(s, "IMPORT")) return MB_SRC_IMPORT;
  return MB_SRC_USER;
}
static mb_counterparty_kind parse_cp_kind(const char *s) {
  if (s && !strcmp(s, "VENDOR")) return MB_CP_VENDOR;
  if (s && !strcmp(s, "BOTH")) return MB_CP_BOTH;
  return MB_CP_CUSTOMER;
}
static const char *cp_kind_str(mb_counterparty_kind k) {
  return k == MB_CP_VENDOR ? "VENDOR" : k == MB_CP_BOTH ? "BOTH" : "CUSTOMER";
}

static mb_err parse_type(const char *s, mb_account_type *out) {
  if (!s) return MB_FAIL(MB_ERR_INVALID_ARG, "type required");
  if (!strcmp(s, "ASSET"))     { *out = MB_ACCT_ASSET;     return MB_OK; }
  if (!strcmp(s, "LIABILITY")) { *out = MB_ACCT_LIABILITY; return MB_OK; }
  if (!strcmp(s, "EQUITY"))    { *out = MB_ACCT_EQUITY;    return MB_OK; }
  if (!strcmp(s, "INCOME"))    { *out = MB_ACCT_INCOME;    return MB_OK; }
  if (!strcmp(s, "EXPENSE"))   { *out = MB_ACCT_EXPENSE;   return MB_OK; }
  return MB_FAIL(MB_ERR_INVALID_ARG, "bad type '%s'", s);
}
static mb_err parse_role(const char *s, mb_account_role *out) {
  if (!s) return MB_FAIL(MB_ERR_INVALID_ARG, "role required");
  if (!strcmp(s, "SYSTEM"))   { *out = MB_ROLE_SYSTEM;   return MB_OK; }
  if (!strcmp(s, "ACCOUNT"))  { *out = MB_ROLE_ACCOUNT;  return MB_OK; }
  if (!strcmp(s, "CATEGORY")) { *out = MB_ROLE_CATEGORY; return MB_OK; }
  return MB_FAIL(MB_ERR_INVALID_ARG, "bad role '%s'", s);
}

static void add_account_json(cJSON *arr_or_obj, const mb_account *a, int is_array) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "id", a->id);
  cJSON_AddStringToObject(o, "code", a->code);
  cJSON_AddStringToObject(o, "name", a->name);
  cJSON_AddStringToObject(o, "type", mb_account_type_str(a->type));
  cJSON_AddStringToObject(o, "role", mb_account_role_str(a->role));
  cJSON_AddStringToObject(o, "currency", a->currency);
  cJSON_AddBoolToObject(o, "is_active", a->is_active);
  if (is_array) cJSON_AddItemToArray(arr_or_obj, o);
  else cJSON_AddItemToObject(arr_or_obj, "account", o);
}

/* ---- handlers: fill *res (a new cJSON object) or return an error ---- */
static mb_err h_account_create(mb_store *s, const cJSON *a, cJSON **res) {
  mb_account_type type;
  mb_account_role role;
  MB_TRY(parse_type(jstr(a, "type"), &type));
  MB_TRY(parse_role(jstr(a, "role"), &role));
  mb_account_new in = {.code = jstr(a, "code"), .name = jstr(a, "name"),
                       .type = type, .role = role, .parent_id = jstr(a, "parent_id"),
                       .currency = jstr(a, "currency")};
  char id[40];
  MB_TRY(mb_account_create(s, &in, id));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "id", id);
  return MB_OK;
}

static mb_err h_account_get(mb_store *s, const cJSON *a, cJSON **res) {
  const char *id = jstr(a, "id");
  if (!id) return MB_FAIL(MB_ERR_INVALID_ARG, "id required");
  mb_account acc;
  MB_TRY(mb_account_get(s, id, &acc));
  *res = cJSON_CreateObject();
  add_account_json(*res, &acc, 0);
  return MB_OK;
}

static mb_err h_account_list(mb_store *s, const cJSON *a, cJSON **res) {
  mb_account_filter f = {0};
  const char *ts = jstr(a, "type");
  const char *rs = jstr(a, "role");
  if (ts) { MB_TRY(parse_type(ts, &f.type)); f.has_type = 1; }
  if (rs) { MB_TRY(parse_role(rs, &f.role)); f.has_role = 1; }
  f.active_only = jbool(a, "active_only");
  mb_account *rows = NULL;
  int n = 0;
  MB_TRY(mb_account_list(s, &f, &rows, &n));
  *res = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(*res, "accounts");
  for (int i = 0; i < n; i++) add_account_json(arr, &rows[i], 1);
  free(rows);
  return MB_OK;
}
static mb_err h_account_update(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_account_update(s, jstr(a, "id"), jstr(a, "code"), jstr(a, "name")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}
static mb_err h_account_set_active(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_account_set_active(s, jstr(a, "id"), jbool(a, "active")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}

/* ---- items (service/expense dictionary, D14) ---- */
static mb_item_kind parse_item_kind(const char *s) {
  return (s && !strcmp(s, "EXPENSE")) ? MB_ITEM_EXPENSE : MB_ITEM_SERVICE;
}
static void add_item_json(cJSON *arr, const mb_item *it) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "id", it->id);
  cJSON_AddStringToObject(o, "kind", mb_item_kind_str(it->kind));
  cJSON_AddStringToObject(o, "name", it->name);
  cJSON_AddNumberToObject(o, "default_unit_price", (double)it->default_unit_price);
  cJSON_AddStringToObject(o, "default_account_id", it->default_account_id);
  cJSON_AddStringToObject(o, "unit_label", it->unit_label);
  cJSON_AddBoolToObject(o, "is_active", it->is_active);
  cJSON_AddItemToArray(arr, o);
}
static mb_err h_item_create(mb_store *s, const cJSON *a, cJSON **res) {
  mb_item_new in = {.kind = parse_item_kind(jstr(a, "kind")), .name = jstr(a, "name"),
                    .default_unit_price = (mb_money)jint(a, "default_unit_price", 0),
                    .default_account_id = jstr(a, "default_account_id"),
                    .unit_label = jstr(a, "unit_label")};
  char id[40];
  MB_TRY(mb_item_create(s, &in, id));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "id", id);
  return MB_OK;
}
static mb_err h_item_list(mb_store *s, const cJSON *a, cJSON **res) {
  mb_item *rows = NULL;
  int n = 0;
  MB_TRY(mb_item_list(s, jbool(a, "active_only"), &rows, &n));
  *res = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(*res, "items");
  for (int i = 0; i < n; i++) add_item_json(arr, &rows[i]);
  free(rows);
  return MB_OK;
}
static mb_err h_item_set_active(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_item_set_active(s, jstr(a, "id"), jbool(a, "active")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}

static mb_err h_trial_balance(mb_store *s, const cJSON *a, cJSON **res) {
  mb_acct_balance *rows = NULL;
  int n = 0;
  mb_trial_balance tb;
  MB_TRY(mb_report_trial_balance(s, jstr(a, "as_of"), &rows, &n, &tb));
  *res = cJSON_CreateObject();
  cJSON_AddNumberToObject(*res, "total_debit", (double)tb.total_debit);
  cJSON_AddNumberToObject(*res, "total_credit", (double)tb.total_credit);
  cJSON_AddBoolToObject(*res, "balanced", tb.balanced);
  cJSON *arr = cJSON_AddArrayToObject(*res, "rows");
  for (int i = 0; i < n; i++) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "code", rows[i].code);
    cJSON_AddStringToObject(r, "name", rows[i].name);
    cJSON_AddStringToObject(r, "type", mb_account_type_str(rows[i].type));
    cJSON_AddNumberToObject(r, "balance", (double)rows[i].balance);
    cJSON_AddItemToArray(arr, r);
  }
  free(rows);
  return MB_OK;
}

static mb_err h_pnl(mb_store *s, const cJSON *a, cJSON **res) {
  mb_pnl pl;
  MB_TRY(mb_report_pnl(s, jstr(a, "from"), jstr(a, "to"), &pl));
  *res = cJSON_CreateObject();
  cJSON_AddNumberToObject(*res, "income", (double)pl.income);
  cJSON_AddNumberToObject(*res, "expense", (double)pl.expense);
  cJSON_AddNumberToObject(*res, "net", (double)pl.net);
  return MB_OK;
}

static mb_err h_balance_sheet(mb_store *s, const cJSON *a, cJSON **res) {
  mb_balance_sheet bs;
  MB_TRY(mb_report_balance_sheet(s, jstr(a, "as_of"), &bs));
  *res = cJSON_CreateObject();
  cJSON_AddNumberToObject(*res, "assets", (double)bs.assets);
  cJSON_AddNumberToObject(*res, "liabilities", (double)bs.liabilities);
  cJSON_AddNumberToObject(*res, "equity", (double)bs.equity);
  cJSON_AddNumberToObject(*res, "net_income", (double)bs.net_income);
  cJSON_AddBoolToObject(*res, "balanced", bs.balanced);
  return MB_OK;
}

static mb_err h_cash_flow(mb_store *s, const cJSON *a, cJSON **res) {
  mb_cash_flow cf;
  MB_TRY(mb_report_cash_flow(s, jstr(a, "from"), jstr(a, "to"), &cf));
  *res = cJSON_CreateObject();
  cJSON_AddNumberToObject(*res, "inflow", (double)cf.inflow);
  cJSON_AddNumberToObject(*res, "outflow", (double)cf.outflow);
  cJSON_AddNumberToObject(*res, "net", (double)cf.net);
  return MB_OK;
}

static mb_err h_report_journal(mb_store *s, const cJSON *a, cJSON **res) {
  mb_journal_row *rows = NULL;
  int n = 0;
  MB_TRY(mb_report_journal(s, jstr(a, "from"), jstr(a, "to"), &rows, &n));
  *res = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(*res, "entries");
  for (int i = 0; i < n; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "entry_id", rows[i].entry_id);
    cJSON_AddStringToObject(o, "date", rows[i].date);
    cJSON_AddStringToObject(o, "memo", rows[i].memo);
    cJSON_AddStringToObject(o, "source", rows[i].source);
    cJSON_AddStringToObject(o, "status", rows[i].status);
    cJSON_AddStringToObject(o, "flow", rows[i].flow);
    cJSON_AddNumberToObject(o, "amount", (double)rows[i].amount);
    cJSON_AddItemToArray(arr, o);
  }
  free(rows);
  return MB_OK;
}

static mb_err h_report_category_txns(mb_store *s, const cJSON *a, cJSON **res) {
  const char *type = jstr(a, "type");
  if (!type || (strcmp(type, "INCOME") && strcmp(type, "EXPENSE")))
    return MB_FAIL(MB_ERR_INVALID_ARG, "type must be INCOME or EXPENSE");
  mb_cat_txn_row *rows = NULL;
  int n = 0;
  MB_TRY(mb_report_category_txns(s, type, jstr(a, "from"), jstr(a, "to"), &rows, &n));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "type", type);
  cJSON *arr = cJSON_AddArrayToObject(*res, "transactions");
  mb_money sum = 0;
  for (int i = 0; i < n; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "entry_id", rows[i].entry_id);
    cJSON_AddStringToObject(o, "date", rows[i].date);
    cJSON_AddStringToObject(o, "memo", rows[i].memo);
    cJSON_AddStringToObject(o, "category_id", rows[i].category_id);
    cJSON_AddStringToObject(o, "category_name", rows[i].category_name);
    cJSON_AddNumberToObject(o, "amount", (double)rows[i].amount);
    cJSON_AddItemToArray(arr, o);
    sum += rows[i].amount;
  }
  cJSON_AddNumberToObject(*res, "total", (double)sum);
  free(rows);
  return MB_OK;
}

static mb_err h_report_ledger(mb_store *s, const cJSON *a, cJSON **res) {
  const char *acct = jstr(a, "account_id");
  if (!acct) return MB_FAIL(MB_ERR_INVALID_ARG, "account_id required");
  mb_ledger_row *rows = NULL;
  int n = 0;
  MB_TRY(mb_report_ledger(s, acct, jstr(a, "from"), jstr(a, "to"), &rows, &n));
  *res = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(*res, "rows");
  for (int i = 0; i < n; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "entry_id", rows[i].entry_id);
    cJSON_AddStringToObject(o, "date", rows[i].date);
    cJSON_AddStringToObject(o, "memo", rows[i].memo);
    cJSON_AddNumberToObject(o, "amount", (double)rows[i].amount);
    cJSON_AddNumberToObject(o, "running", (double)rows[i].running);
    cJSON_AddItemToArray(arr, o);
  }
  free(rows);
  return MB_OK;
}

/* ---- export handlers (read-only; CSV for accountant sharing) ---- */
static mb_err export_result(cJSON **res, const char *filename, char *csv) {
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "filename", filename);
  cJSON_AddStringToObject(*res, "mime", "text/csv");
  cJSON_AddStringToObject(*res, "content", csv);   /* duplicated by cJSON */
  free(csv);
  return MB_OK;
}

static mb_err h_export_trial_balance(mb_store *s, const cJSON *a, cJSON **res) {
  char *csv = NULL;
  MB_TRY(mb_export_trial_balance_csv(s, jstr(a, "as_of"), &csv));
  return export_result(res, "trial_balance.csv", csv);
}
static mb_err h_export_pnl(mb_store *s, const cJSON *a, cJSON **res) {
  char *csv = NULL;
  MB_TRY(mb_export_pnl_csv(s, jstr(a, "from"), jstr(a, "to"), &csv));
  return export_result(res, "profit_and_loss.csv", csv);
}
static mb_err h_export_balance_sheet(mb_store *s, const cJSON *a, cJSON **res) {
  char *csv = NULL;
  MB_TRY(mb_export_balance_sheet_csv(s, jstr(a, "as_of"), &csv));
  return export_result(res, "balance_sheet.csv", csv);
}
static mb_err h_export_general_ledger(mb_store *s, const cJSON *a, cJSON **res) {
  char *csv = NULL;
  MB_TRY(mb_export_general_ledger_csv(s, jstr(a, "from"), jstr(a, "to"), &csv));
  return export_result(res, "general_ledger.csv", csv);
}
static mb_err h_export_journal(mb_store *s, const cJSON *a, cJSON **res) {
  char *csv = NULL;
  MB_TRY(mb_export_journal_csv(s, jstr(a, "from"), jstr(a, "to"), &csv));
  return export_result(res, "journal.csv", csv);
}

/* ---- write handlers ---- */
static mb_err h_transaction_post(mb_store *s, const cJSON *a, cJSON **res) {
  const cJSON *ps = cJSON_GetObjectItemCaseSensitive(a, "postings");
  if (!cJSON_IsArray(ps)) return MB_FAIL(MB_ERR_INVALID_ARG, "postings array required");
  int n = cJSON_GetArraySize(ps);
  if (n < 2) return MB_FAIL(MB_ERR_INVALID_ARG, "need >= 2 postings");
  mb_posting_in *p = malloc((size_t)n * sizeof *p);
  if (!p) return MB_FAIL(MB_ERR_INTERNAL, "oom");
  for (int i = 0; i < n; i++) {
    const cJSON *it = cJSON_GetArrayItem(ps, i);
    p[i].account_id = jstr(it, "account_id");      /* valid until args is deleted */
    p[i].amount = (mb_money)jint(it, "amount", 0);
    p[i].memo = jstr(it, "memo");
    p[i].counterparty_id = jstr(it, "counterparty_id");  /* nullable; must be set — struct isn't zeroed */
    if (!p[i].account_id) { free(p); return MB_FAIL(MB_ERR_INVALID_ARG, "posting %d missing account_id", i); }
  }
  char eid[40];
  mb_err e = mb_journal_post(s, jstr(a, "date"), jstr(a, "memo"),
                             parse_source(jstr(a, "source")), p, n, eid);
  free(p);
  if (e != MB_OK) return e;
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "entry_id", eid);
  return MB_OK;
}

/* income: Dr deposit account, Cr income category */
static mb_err h_income_record(mb_store *s, const cJSON *a, cJSON **res) {
  mb_money amt = (mb_money)jint(a, "amount", 0);
  const char *dep = jstr(a, "deposit_account_id"), *cat = jstr(a, "category_id");
  if (amt <= 0) return MB_FAIL(MB_ERR_INVALID_ARG, "amount must be positive");
  if (!dep || !cat) return MB_FAIL(MB_ERR_INVALID_ARG, "deposit_account_id and category_id required");
  mb_posting_in p[2] = {{.account_id = dep, .amount = amt}, {.account_id = cat, .amount = -amt}};
  char eid[40];
  MB_TRY(mb_journal_post(s, jstr(a, "date"), jstr(a, "memo"), parse_source(jstr(a, "source")), p, 2, eid));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "entry_id", eid);
  return MB_OK;
}

/* expense: Dr expense category, Cr pay account */
static mb_err h_expense_record(mb_store *s, const cJSON *a, cJSON **res) {
  mb_money amt = (mb_money)jint(a, "amount", 0);
  const char *pay = jstr(a, "pay_account_id"), *cat = jstr(a, "category_id");
  if (amt <= 0) return MB_FAIL(MB_ERR_INVALID_ARG, "amount must be positive");
  if (!pay || !cat) return MB_FAIL(MB_ERR_INVALID_ARG, "pay_account_id and category_id required");
  mb_posting_in p[2] = {{.account_id = cat, .amount = amt}, {.account_id = pay, .amount = -amt}};
  char eid[40];
  MB_TRY(mb_journal_post(s, jstr(a, "date"), jstr(a, "memo"), parse_source(jstr(a, "source")), p, 2, eid));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "entry_id", eid);
  return MB_OK;
}

static mb_err h_counterparty_create(mb_store *s, const cJSON *a, cJSON **res) {
  mb_counterparty_new in = {.name = jstr(a, "name"), .kind = parse_cp_kind(jstr(a, "kind")),
                            .email = jstr(a, "email"), .phone = jstr(a, "phone"),
                            .address = jstr(a, "address"), .note = jstr(a, "note")};
  char id[40];
  MB_TRY(mb_counterparty_create(s, &in, id));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "id", id);
  return MB_OK;
}

static mb_err h_counterparty_list(mb_store *s, const cJSON *a, cJSON **res) {
  mb_counterparty *rows = NULL;
  int n = 0;
  MB_TRY(mb_counterparty_list(s, jbool(a, "active_only"), &rows, &n));
  *res = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(*res, "counterparties");
  for (int i = 0; i < n; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", rows[i].id);
    cJSON_AddStringToObject(o, "name", rows[i].name);
    cJSON_AddStringToObject(o, "kind", cp_kind_str(rows[i].kind));
    cJSON_AddItemToArray(arr, o);
  }
  free(rows);
  return MB_OK;
}

static mb_err h_invoice_create(mb_store *s, const cJSON *a, cJSON **res) {
  char id[40];
  MB_TRY(mb_invoice_create(s, jstr(a, "counterparty_id"), jstr(a, "number"),
                           jstr(a, "due_date"), jstr(a, "memo"), id));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "id", id);
  return MB_OK;
}

static mb_err h_invoice_add_line(mb_store *s, const cJSON *a, cJSON **res) {
  mb_invoice_line_in in = {.item_id = jstr(a, "item_id"), .description = jstr(a, "description"),
                           .qty_centi = jint(a, "qty_centi", 0),
                           .unit_price = (mb_money)jint(a, "unit_price", 0),
                           .account_id = jstr(a, "account_id"), .is_tax = jbool(a, "is_tax")};
  char lid[40];
  MB_TRY(mb_invoice_add_line(s, jstr(a, "invoice_id"), &in, lid));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "line_id", lid);
  return MB_OK;
}

static mb_err h_invoice_issue(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_invoice_issue(s, jstr(a, "invoice_id"), jstr(a, "issue_date")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}

static void add_line_obj(cJSON *arr, const char *id, const char *desc, int64_t qty_centi,
                         mb_money unit_price, mb_money line_total, const char *acct_id,
                         const char *acct_name, int is_tax) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "id", id);
  cJSON_AddStringToObject(o, "description", desc);
  cJSON_AddNumberToObject(o, "qty_centi", (double)qty_centi);
  cJSON_AddNumberToObject(o, "unit_price", (double)unit_price);
  cJSON_AddNumberToObject(o, "line_total", (double)line_total);
  cJSON_AddStringToObject(o, "account_id", acct_id);
  cJSON_AddStringToObject(o, "account_name", acct_name);
  cJSON_AddBoolToObject(o, "is_tax", is_tax);
  cJSON_AddItemToArray(arr, o);
}

static mb_err h_invoice_get(mb_store *s, const cJSON *a, cJSON **res) {
  mb_invoice inv;
  MB_TRY(mb_invoice_get(s, jstr(a, "id"), &inv));
  mb_money total = 0;
  (void)mb_invoice_total(s, inv.id, &total);
  char cp_name[128] = "";
  mb_counterparty cp;
  if (mb_counterparty_get(s, inv.counterparty_id, &cp) == MB_OK)
    snprintf(cp_name, sizeof cp_name, "%s", cp.name);
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "id", inv.id);
  cJSON_AddStringToObject(*res, "number", inv.number);
  cJSON_AddStringToObject(*res, "counterparty_id", inv.counterparty_id);
  cJSON_AddStringToObject(*res, "counterparty_name", cp_name);
  cJSON_AddStringToObject(*res, "issue_date", inv.issue_date);
  cJSON_AddStringToObject(*res, "due_date", inv.due_date);
  cJSON_AddStringToObject(*res, "memo", inv.memo);
  cJSON_AddStringToObject(*res, "status", mb_invoice_status_str(inv.status));
  cJSON_AddNumberToObject(*res, "total", (double)total);
  cJSON *arr = cJSON_AddArrayToObject(*res, "lines");
  mb_invoice_line *lines = NULL;
  int ln = 0;
  if (mb_invoice_lines(s, inv.id, &lines, &ln) == MB_OK) {
    for (int i = 0; i < ln; i++)
      add_line_obj(arr, lines[i].id, lines[i].description, lines[i].qty_centi, lines[i].unit_price,
                   lines[i].line_total, lines[i].account_id, lines[i].account_name, lines[i].is_tax);
    free(lines);
  }
  return MB_OK;
}

static mb_err h_bill_get(mb_store *s, const cJSON *a, cJSON **res) {
  mb_bill b;
  MB_TRY(mb_bill_get(s, jstr(a, "id"), &b));
  mb_money total = 0;
  (void)mb_bill_total(s, b.id, &total);
  char cp_name[128] = "";
  mb_counterparty cp;
  if (mb_counterparty_get(s, b.counterparty_id, &cp) == MB_OK)
    snprintf(cp_name, sizeof cp_name, "%s", cp.name);
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "id", b.id);
  cJSON_AddStringToObject(*res, "number", b.number);
  cJSON_AddStringToObject(*res, "counterparty_id", b.counterparty_id);
  cJSON_AddStringToObject(*res, "counterparty_name", cp_name);
  cJSON_AddStringToObject(*res, "issue_date", b.issue_date);
  cJSON_AddStringToObject(*res, "due_date", b.due_date);
  cJSON_AddStringToObject(*res, "memo", b.memo);
  cJSON_AddStringToObject(*res, "status", mb_bill_status_str(b.status));
  cJSON_AddNumberToObject(*res, "total", (double)total);
  cJSON *arr = cJSON_AddArrayToObject(*res, "lines");
  mb_bill_line *lines = NULL;
  int ln = 0;
  if (mb_bill_lines(s, b.id, &lines, &ln) == MB_OK) {
    for (int i = 0; i < ln; i++)
      add_line_obj(arr, lines[i].id, lines[i].description, lines[i].qty_centi, lines[i].unit_price,
                   lines[i].line_total, lines[i].account_id, lines[i].account_name, lines[i].is_tax);
    free(lines);
  }
  return MB_OK;
}

static mb_err h_invoice_remove_line(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_invoice_remove_line(s, jstr(a, "line_id")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}
static mb_err h_invoice_update(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_invoice_update(s, jstr(a, "id"), jstr(a, "number"), jstr(a, "due_date"), jstr(a, "memo")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}
static mb_err h_invoice_void(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_invoice_void(s, jstr(a, "id")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}
static mb_err h_bill_remove_line(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_bill_remove_line(s, jstr(a, "line_id")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}
static mb_err h_bill_update(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_bill_update(s, jstr(a, "id"), jstr(a, "number"), jstr(a, "due_date"), jstr(a, "memo")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}
static mb_err h_bill_void(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_bill_void(s, jstr(a, "id")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}

static mb_err h_bill_create(mb_store *s, const cJSON *a, cJSON **res) {
  char id[40];
  MB_TRY(mb_bill_create(s, jstr(a, "counterparty_id"), jstr(a, "number"),
                        jstr(a, "due_date"), jstr(a, "memo"), id));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "id", id);
  return MB_OK;
}

static mb_err h_bill_add_line(mb_store *s, const cJSON *a, cJSON **res) {
  mb_bill_line_in in = {.item_id = jstr(a, "item_id"), .description = jstr(a, "description"),
                        .qty_centi = jint(a, "qty_centi", 0),
                        .unit_price = (mb_money)jint(a, "unit_price", 0),
                        .account_id = jstr(a, "account_id"), .is_tax = jbool(a, "is_tax")};
  char lid[40];
  MB_TRY(mb_bill_add_line(s, jstr(a, "bill_id"), &in, lid));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "line_id", lid);
  return MB_OK;
}

static mb_err h_bill_enter(mb_store *s, const cJSON *a, cJSON **res) {
  MB_TRY(mb_bill_enter(s, jstr(a, "bill_id"), jstr(a, "issue_date")));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}

static mb_err h_payment_record(mb_store *s, const cJSON *a, cJSON **res) {
  const char *t = jstr(a, "target");
  mb_pay_target target = (t && !strcmp(t, "BILL")) ? MB_PAY_BILL : MB_PAY_INVOICE;
  char id[40];
  MB_TRY(mb_payment_record(s, jstr(a, "date"), (mb_money)jint(a, "amount", 0),
                           jstr(a, "cash_account_id"), target, jstr(a, "target_id"), id));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "id", id);
  return MB_OK;
}

/* D26: manually apply available counterparty credit to an open document */
static mb_err h_credit_apply(mb_store *s, const cJSON *a, cJSON **res) {
  const char *t = jstr(a, "target");
  mb_pay_target target = (t && !strcmp(t, "BILL")) ? MB_PAY_BILL : MB_PAY_INVOICE;
  char id[40];
  MB_TRY(mb_credit_apply(s, jstr(a, "date"), jstr(a, "counterparty_id"), target,
                         jstr(a, "target_id"), (mb_money)jint(a, "amount", 0), id));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "id", id);
  return MB_OK;
}

/* D26: a counterparty's running balance + available credit for one side (AR or AP) */
static mb_err h_counterparty_balance(mb_store *s, const cJSON *a, cJSON **res) {
  const char *t = jstr(a, "target");
  mb_pay_target target = (t && !strcmp(t, "BILL")) ? MB_PAY_BILL : MB_PAY_INVOICE;
  const char *cp = jstr(a, "counterparty_id");
  mb_money bal = 0, credit = 0;
  MB_TRY(mb_counterparty_balance(s, cp, target, &bal));
  MB_TRY(mb_credit_available(s, cp, target, &credit));
  *res = cJSON_CreateObject();
  cJSON_AddStringToObject(*res, "counterparty_id", cp ? cp : "");
  cJSON_AddNumberToObject(*res, "balance", (double)bal);
  cJSON_AddNumberToObject(*res, "credit_available", (double)credit);
  return MB_OK;
}

static mb_err h_invoice_list(mb_store *s, const cJSON *a, cJSON **res) {
  (void)a;
  mb_invoice_row *rows = NULL;
  int n = 0;
  MB_TRY(mb_invoice_list(s, &rows, &n));
  *res = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(*res, "invoices");
  for (int i = 0; i < n; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", rows[i].id);
    cJSON_AddStringToObject(o, "number", rows[i].number);
    cJSON_AddStringToObject(o, "counterparty_id", rows[i].counterparty_id);
    cJSON_AddStringToObject(o, "counterparty_name", rows[i].counterparty_name);
    cJSON_AddStringToObject(o, "issue_date", rows[i].issue_date);
    cJSON_AddStringToObject(o, "due_date", rows[i].due_date);
    cJSON_AddStringToObject(o, "status", mb_invoice_status_str(rows[i].status));
    cJSON_AddNumberToObject(o, "total", (double)rows[i].total);
    cJSON_AddItemToArray(arr, o);
  }
  free(rows);
  return MB_OK;
}

static mb_err h_bill_list(mb_store *s, const cJSON *a, cJSON **res) {
  (void)a;
  mb_bill_row *rows = NULL;
  int n = 0;
  MB_TRY(mb_bill_list(s, &rows, &n));
  *res = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(*res, "bills");
  for (int i = 0; i < n; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", rows[i].id);
    cJSON_AddStringToObject(o, "number", rows[i].number);
    cJSON_AddStringToObject(o, "counterparty_id", rows[i].counterparty_id);
    cJSON_AddStringToObject(o, "counterparty_name", rows[i].counterparty_name);
    cJSON_AddStringToObject(o, "issue_date", rows[i].issue_date);
    cJSON_AddStringToObject(o, "due_date", rows[i].due_date);
    cJSON_AddStringToObject(o, "status", mb_bill_status_str(rows[i].status));
    cJSON_AddNumberToObject(o, "total", (double)rows[i].total);
    cJSON_AddItemToArray(arr, o);
  }
  free(rows);
  return MB_OK;
}

/* ---- first-run onboarding (D7): the UI wizard, not a silent auto-seed ---- */
static mb_err h_book_status(mb_store *s, const cJSON *a, cJSON **res) {
  (void)a;
  char buf[8] = {0};
  int flag = (app_setting_get(s, "onboarded", buf, sizeof buf) == MB_OK && !strcmp(buf, "1"));
  int n = 0;
  (void)mb_account_count(s, &n);
  char company[128] = "";
  (void)mb_book_company_name(s, company, sizeof company);
  /* A book seeded before the flag existed (or seeded any other way) is already set up —
   * treat any non-empty chart as onboarded so we never re-show the wizard or re-seed. */
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "onboarded", flag || n > 0);
  cJSON_AddNumberToObject(*res, "account_count", n);
  cJSON_AddStringToObject(*res, "company_name", company);
  return MB_OK;
}

static mb_err h_book_set_name(mb_store *s, const cJSON *a, cJSON **res) {
  const char *name = jstr(a, "name");
  if (!name || !name[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "name required");
  MB_TRY(mb_book_set_company_name(s, name));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}

static mb_err h_book_onboard(mb_store *s, const cJSON *a, cJSON **res) {
  int n = 0;
  (void)mb_account_count(s, &n);
  if (n == 0) {   /* only seed an empty book; seeding is not idempotent */
    const char *tmpl = jstr(a, "template");
    if (tmpl && !strcmp(tmpl, "freelancer")) MB_TRY(mb_seed_starter_chart(s));
    else                                     MB_TRY(mb_seed_system_accounts(s));
  }
  MB_TRY(app_setting_set(s, "onboarded", "1"));
  *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(*res, "ok", 1);
  return MB_OK;
}

/* ---- app settings (generic key/value store in app_setting; e.g. the onboarding flag) ---- */
static mb_err app_setting_get(mb_store *s, const char *k, char *buf, size_t n) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s), "SELECT v FROM app_setting WHERE k=?;", -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, k, -1, SQLITE_TRANSIENT);
  mb_err e;
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW)       { snprintf(buf, n, "%s", (const char *)sqlite3_column_text(st, 0)); e = MB_OK; }
  else if (rc == SQLITE_DONE) { e = MB_FAIL(MB_ERR_NOT_FOUND, "setting '%s'", k); }
  else                        { e = MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s))); }
  sqlite3_finalize(st);
  return e;
}

static mb_err app_setting_set(mb_store *s, const char *k, const char *v) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(mb_store_handle(s),
        "INSERT INTO app_setting(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v;",
        -1, &st, NULL) != SQLITE_OK)
    return MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_bind_text(st, 1, k, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, v, -1, SQLITE_TRANSIENT);
  mb_err e = (sqlite3_step(st) == SQLITE_DONE) ? MB_OK : MB_FAIL(MB_ERR_DB, "%s", sqlite3_errmsg(mb_store_handle(s)));
  sqlite3_finalize(st);
  return e;
}

/* MCP tool catalog for the in-app "what does my MCP expose?" view (not itself an MCP tool). */
static mb_err h_mcp_tools(mb_store *s, const cJSON *a, cJSON **res) {
  (void)a;
  char *cat = NULL;
  MB_TRY(mb_mcp_tools_catalog(s, &cat));
  *res = cJSON_Parse(cat);
  free(cat);
  return *res ? MB_OK : MB_FAIL(MB_ERR_INTERNAL, "parse catalog");
}

mb_err mb_api_dispatch(mb_store *s, const char *method, const char *args_json, char **result_json) {
  cJSON *args = (args_json && args_json[0]) ? cJSON_Parse(args_json) : cJSON_CreateObject();
  if (!args) {
    *result_json = cJSON_PrintUnformatted(
        cJSON_Parse("{\"error\":{\"code\":\"MB_ERR_PARSE\",\"message\":\"invalid args JSON\"}}"));
    return MB_ERR_PARSE;
  }

  cJSON *res = NULL;
  mb_err e;
  if      (!strcmp(method, "account.create"))      e = h_account_create(s, args, &res);
  else if (!strcmp(method, "account.get"))         e = h_account_get(s, args, &res);
  else if (!strcmp(method, "account.list"))        e = h_account_list(s, args, &res);
  else if (!strcmp(method, "account.update"))      e = h_account_update(s, args, &res);
  else if (!strcmp(method, "account.set_active"))  e = h_account_set_active(s, args, &res);
  else if (!strcmp(method, "item.create"))         e = h_item_create(s, args, &res);
  else if (!strcmp(method, "item.list"))           e = h_item_list(s, args, &res);
  else if (!strcmp(method, "item.set_active"))     e = h_item_set_active(s, args, &res);
  else if (!strcmp(method, "report.trial_balance"))e = h_trial_balance(s, args, &res);
  else if (!strcmp(method, "report.pnl"))          e = h_pnl(s, args, &res);
  else if (!strcmp(method, "report.balance_sheet"))e = h_balance_sheet(s, args, &res);
  else if (!strcmp(method, "report.cash_flow"))    e = h_cash_flow(s, args, &res);
  else if (!strcmp(method, "report.journal"))      e = h_report_journal(s, args, &res);
  else if (!strcmp(method, "report.category_txns"))e = h_report_category_txns(s, args, &res);
  else if (!strcmp(method, "report.ledger"))       e = h_report_ledger(s, args, &res);
  else if (!strcmp(method, "export.trial_balance"))e = h_export_trial_balance(s, args, &res);
  else if (!strcmp(method, "export.pnl"))           e = h_export_pnl(s, args, &res);
  else if (!strcmp(method, "export.balance_sheet")) e = h_export_balance_sheet(s, args, &res);
  else if (!strcmp(method, "export.general_ledger"))e = h_export_general_ledger(s, args, &res);
  else if (!strcmp(method, "export.journal"))       e = h_export_journal(s, args, &res);
  else if (!strcmp(method, "transaction.post"))    e = h_transaction_post(s, args, &res);
  else if (!strcmp(method, "income.record"))       e = h_income_record(s, args, &res);
  else if (!strcmp(method, "expense.record"))      e = h_expense_record(s, args, &res);
  else if (!strcmp(method, "counterparty.create")) e = h_counterparty_create(s, args, &res);
  else if (!strcmp(method, "counterparty.list"))   e = h_counterparty_list(s, args, &res);
  else if (!strcmp(method, "invoice.create"))      e = h_invoice_create(s, args, &res);
  else if (!strcmp(method, "invoice.add_line"))    e = h_invoice_add_line(s, args, &res);
  else if (!strcmp(method, "invoice.issue"))       e = h_invoice_issue(s, args, &res);
  else if (!strcmp(method, "invoice.get"))         e = h_invoice_get(s, args, &res);
  else if (!strcmp(method, "invoice.remove_line")) e = h_invoice_remove_line(s, args, &res);
  else if (!strcmp(method, "invoice.update"))      e = h_invoice_update(s, args, &res);
  else if (!strcmp(method, "invoice.void"))        e = h_invoice_void(s, args, &res);
  else if (!strcmp(method, "bill.create"))         e = h_bill_create(s, args, &res);
  else if (!strcmp(method, "bill.add_line"))       e = h_bill_add_line(s, args, &res);
  else if (!strcmp(method, "bill.enter"))          e = h_bill_enter(s, args, &res);
  else if (!strcmp(method, "bill.get"))            e = h_bill_get(s, args, &res);
  else if (!strcmp(method, "bill.remove_line"))    e = h_bill_remove_line(s, args, &res);
  else if (!strcmp(method, "bill.update"))         e = h_bill_update(s, args, &res);
  else if (!strcmp(method, "bill.void"))           e = h_bill_void(s, args, &res);
  else if (!strcmp(method, "bill.list"))           e = h_bill_list(s, args, &res);
  else if (!strcmp(method, "invoice.list"))        e = h_invoice_list(s, args, &res);
  else if (!strcmp(method, "payment.record"))      e = h_payment_record(s, args, &res);
  else if (!strcmp(method, "credit.apply"))        e = h_credit_apply(s, args, &res);
  else if (!strcmp(method, "counterparty.balance"))e = h_counterparty_balance(s, args, &res);
  else if (!strcmp(method, "book.status"))         e = h_book_status(s, args, &res);
  else if (!strcmp(method, "book.onboard"))        e = h_book_onboard(s, args, &res);
  else if (!strcmp(method, "book.set_name"))       e = h_book_set_name(s, args, &res);
  else if (!strcmp(method, "mcp.tools"))           e = h_mcp_tools(s, args, &res);
  else                                             e = MB_FAIL(MB_ERR_UNSUPPORTED, "unknown method '%s'", method);

  cJSON_Delete(args);

  if (e == MB_OK) {
    *result_json = cJSON_PrintUnformatted(res);
    cJSON_Delete(res);
  } else {
    if (res) cJSON_Delete(res);
    cJSON *env = cJSON_CreateObject();
    cJSON *err = cJSON_AddObjectToObject(env, "error");
    cJSON_AddStringToObject(err, "code", mb_err_name(e));
    cJSON_AddStringToObject(err, "message", mb_last_error()->message);
    *result_json = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
  }
  if (!*result_json) return MB_FAIL(MB_ERR_INTERNAL, "json print failed");
  return e;
}

#ifdef MB_TEST
#include "../support/mb_test.h"
#include "../journal/journal.h"
#include "../seed/seed.h"

TEST(api, account_create_then_get) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char *r = NULL;
  ASSERT_OK(mb_api_dispatch(s,
      "account.create",
      "{\"code\":\"4000\",\"name\":\"Consulting\",\"type\":\"INCOME\",\"role\":\"CATEGORY\"}", &r));
  cJSON *j = cJSON_Parse(r);
  const char *id = cJSON_GetObjectItem(j, "id")->valuestring;
  char getargs[80];
  snprintf(getargs, sizeof getargs, "{\"id\":\"%s\"}", id);
  cJSON_Delete(j);
  free(r);

  ASSERT_OK(mb_api_dispatch(s, "account.get", getargs, &r));
  j = cJSON_Parse(r);
  cJSON *acc = cJSON_GetObjectItem(j, "account");
  ASSERT_STR_EQ(cJSON_GetObjectItem(acc, "name")->valuestring, "Consulting");
  ASSERT_STR_EQ(cJSON_GetObjectItem(acc, "type")->valuestring, "INCOME");
  cJSON_Delete(j);
  free(r);
  mb_store_close(s);
}

TEST(api, onboarding_seeds_once) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char *r = NULL;

  /* fresh book: not onboarded, no accounts */
  ASSERT_OK(mb_api_dispatch(s, "book.status", "{}", &r));
  cJSON *j = cJSON_Parse(r);
  ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(j, "onboarded")));
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(j, "account_count")->valuedouble, 0);
  cJSON_Delete(j); free(r);

  /* onboard with the starter template */
  ASSERT_OK(mb_api_dispatch(s, "book.onboard", "{\"template\":\"freelancer\"}", &r)); free(r);

  ASSERT_OK(mb_api_dispatch(s, "book.status", "{}", &r));
  j = cJSON_Parse(r);
  ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(j, "onboarded")));
  int after = (long)cJSON_GetObjectItem(j, "account_count")->valuedouble;
  ASSERT_EQ_INT(after, 32);   /* freelancer template = the full starter chart (pinned by seed.starter_chart_full) */
  cJSON_Delete(j); free(r);

  /* onboarding again is a no-op (no duplicate-seed error, count unchanged) */
  ASSERT_OK(mb_api_dispatch(s, "book.onboard", "{\"template\":\"freelancer\"}", &r)); free(r);
  ASSERT_OK(mb_api_dispatch(s, "book.status", "{}", &r));
  j = cJSON_Parse(r);
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(j, "account_count")->valuedouble, after);
  cJSON_Delete(j); free(r);
  mb_store_close(s);
}

TEST(api, pnl_report) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40];
  mb_account_new b = {.code="1000", .name="Bank", .type=MB_ACCT_ASSET, .role=MB_ROLE_ACCOUNT};
  mb_account_new i = {.code="4000", .name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &b, bank));
  ASSERT_OK(mb_account_create(s, &i, income));
  mb_posting_in p[] = {{.account_id=bank, .amount=80000}, {.account_id=income, .amount=-80000}};
  char e[40]; ASSERT_OK(mb_journal_post(s, "2026-05-01", "rev", MB_SRC_USER, p, 2, e));

  char *r = NULL;
  ASSERT_OK(mb_api_dispatch(s, "report.pnl", "{}", &r));
  cJSON *j = cJSON_Parse(r);
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(j, "income")->valuedouble, 80000);
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(j, "net")->valuedouble, 80000);
  cJSON_Delete(j);
  free(r);
  mb_store_close(s);
}

TEST(api, export_csv_through_dispatch) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40];
  mb_account_new b = {.code="1000", .name="Bank", .type=MB_ACCT_ASSET, .role=MB_ROLE_ACCOUNT};
  mb_account_new i = {.code="4000", .name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &b, bank));
  ASSERT_OK(mb_account_create(s, &i, income));
  mb_posting_in p[] = {{.account_id=bank, .amount=80000}, {.account_id=income, .amount=-80000}};
  char e[40]; ASSERT_OK(mb_journal_post(s, "2026-05-01", "rev", MB_SRC_USER, p, 2, e));

  char *r = NULL;
  ASSERT_OK(mb_api_dispatch(s, "export.trial_balance", "{}", &r));
  cJSON *j = cJSON_Parse(r);
  ASSERT_STR_EQ(cJSON_GetObjectItem(j, "filename")->valuestring, "trial_balance.csv");
  ASSERT_STR_EQ(cJSON_GetObjectItem(j, "mime")->valuestring, "text/csv");
  const char *csv = cJSON_GetObjectItem(j, "content")->valuestring;
  ASSERT_TRUE(strstr(csv, "Account Code,Account Name,Type,Debit,Credit") != NULL);
  ASSERT_TRUE(strstr(csv, ",TOTAL,800.00,800.00\n") != NULL);
  cJSON_Delete(j); free(r);

  /* general ledger reaches the postings too */
  ASSERT_OK(mb_api_dispatch(s, "export.general_ledger", "{}", &r));
  j = cJSON_Parse(r);
  ASSERT_TRUE(strstr(cJSON_GetObjectItem(j, "content")->valuestring, "rev") != NULL);
  cJSON_Delete(j); free(r);
  mb_store_close(s);
}

TEST(api, unknown_method_errors) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char *r = NULL;
  ASSERT_ERR(mb_api_dispatch(s, "no.such", "{}", &r), MB_ERR_UNSUPPORTED);
  cJSON *j = cJSON_Parse(r);
  ASSERT(cJSON_GetObjectItem(j, "error") != NULL);
  ASSERT_STR_EQ(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "error"), "code")->valuestring, "MB_ERR_UNSUPPORTED");
  cJSON_Delete(j);
  free(r);
  mb_store_close(s);
}

TEST(api, create_bad_type_errors) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char *r = NULL;
  ASSERT_ERR(mb_api_dispatch(s, "account.create",
      "{\"name\":\"X\",\"type\":\"BOGUS\",\"role\":\"ACCOUNT\"}", &r), MB_ERR_INVALID_ARG);
  free(r);
  mb_store_close(s);
}

/* extract a string field from a result JSON, into out (caller-provided) */
static void grab(const char *json, const char *key, char *out, size_t n) {
  cJSON *j = cJSON_Parse(json);
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
  snprintf(out, n, "%s", (v && cJSON_IsString(v)) ? v->valuestring : "");
  cJSON_Delete(j);
}

/* final running balance of an account, read through the report.ledger API (test helper) */
static mb_money api_ledger_balance(mb_store *s, const char *acct) {
  char args[80], *r = NULL;
  snprintf(args, sizeof args, "{\"account_id\":\"%s\"}", acct);
  if (mb_api_dispatch(s, "report.ledger", args, &r) != MB_OK) { free(r); return -1; }
  cJSON *j = cJSON_Parse(r); free(r);
  cJSON *rows = cJSON_GetObjectItem(j, "rows");
  int n = cJSON_GetArraySize(rows);
  mb_money bal = (n > 0) ? (mb_money)cJSON_GetObjectItem(cJSON_GetArrayItem(rows, n - 1), "running")->valuedouble : 0;
  cJSON_Delete(j);
  return bal;
}

TEST(api, income_expense_flow) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  char bank[40], income[40], sw[40];
  mb_account_new b = {.name="Bank", .type=MB_ACCT_ASSET, .role=MB_ROLE_ACCOUNT};
  mb_account_new i = {.name="Consulting", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  mb_account_new x = {.name="Software", .type=MB_ACCT_EXPENSE, .role=MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_create(s, &b, bank));
  ASSERT_OK(mb_account_create(s, &i, income));
  ASSERT_OK(mb_account_create(s, &x, sw));

  char args[256], *r = NULL;
  snprintf(args, sizeof args,
    "{\"date\":\"2026-08-01\",\"amount\":120000,\"deposit_account_id\":\"%s\",\"category_id\":\"%s\"}", bank, income);
  ASSERT_OK(mb_api_dispatch(s, "income.record", args, &r)); free(r);
  snprintf(args, sizeof args,
    "{\"date\":\"2026-08-02\",\"amount\":30000,\"pay_account_id\":\"%s\",\"category_id\":\"%s\"}", bank, sw);
  ASSERT_OK(mb_api_dispatch(s, "expense.record", args, &r)); free(r);

  ASSERT_OK(mb_api_dispatch(s, "report.pnl", "{}", &r));
  cJSON *j = cJSON_Parse(r);
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(j, "income")->valuedouble, 120000);
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(j, "expense")->valuedouble, 30000);
  ASSERT_EQ_INT((long)cJSON_GetObjectItem(j, "net")->valuedouble, 90000);
  cJSON_Delete(j); free(r);
  mb_store_close(s);
}

TEST(api, invoice_payment_flow) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40], bank[40];
  mb_account_new ai = {.name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  mb_account_new ab = {.name="Bank", .type=MB_ACCT_ASSET, .role=MB_ROLE_ACCOUNT};
  ASSERT_OK(mb_account_create(s, &ai, income));
  ASSERT_OK(mb_account_create(s, &ab, bank));

  char *r = NULL, cp[40], inv[40], args[300];
  ASSERT_OK(mb_api_dispatch(s, "counterparty.create", "{\"name\":\"Acme\",\"kind\":\"CUSTOMER\"}", &r));
  grab(r, "id", cp, sizeof cp); free(r);

  snprintf(args, sizeof args, "{\"counterparty_id\":\"%s\",\"number\":\"INV-1\"}", cp);
  ASSERT_OK(mb_api_dispatch(s, "invoice.create", args, &r));
  grab(r, "id", inv, sizeof inv); free(r);

  snprintf(args, sizeof args,
    "{\"invoice_id\":\"%s\",\"description\":\"Work\",\"unit_price\":100000,\"account_id\":\"%s\"}", inv, income);
  ASSERT_OK(mb_api_dispatch(s, "invoice.add_line", args, &r)); free(r);

  snprintf(args, sizeof args, "{\"invoice_id\":\"%s\",\"issue_date\":\"2026-08-01\"}", inv);
  ASSERT_OK(mb_api_dispatch(s, "invoice.issue", args, &r)); free(r);

  snprintf(args, sizeof args,
    "{\"date\":\"2026-08-05\",\"amount\":100000,\"cash_account_id\":\"%s\",\"target\":\"INVOICE\",\"target_id\":\"%s\"}", bank, inv);
  ASSERT_OK(mb_api_dispatch(s, "payment.record", args, &r)); free(r);

  snprintf(args, sizeof args, "{\"id\":\"%s\"}", inv);
  ASSERT_OK(mb_api_dispatch(s, "invoice.get", args, &r));
  char status[16]; grab(r, "status", status, sizeof status); free(r);
  ASSERT_STR_EQ(status, "PAID");

  /* verify the resulting balances through the API, not just the status flag:
   * AR fully cleared and the bank holds the cash (catches a payment posted to the wrong account
   * or amount that still happens to flip status because paid >= total). */
  char ar[40]; ASSERT_OK(mb_store_meta_get(s, "ar_account_id", ar, sizeof ar));
  ASSERT_MONEY_EQ(api_ledger_balance(s, ar), 0);
  ASSERT_MONEY_EQ(api_ledger_balance(s, bank), 100000);
  ASSERT_MONEY_EQ(api_ledger_balance(s, income), -100000);
  mb_store_close(s);
}

TEST(api, credit_overpay_and_apply_flow) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  char income[40], bank[40];
  mb_account_new ai = {.name="Income", .type=MB_ACCT_INCOME, .role=MB_ROLE_CATEGORY};
  mb_account_new ab = {.name="Bank", .type=MB_ACCT_ASSET, .role=MB_ROLE_ACCOUNT};
  ASSERT_OK(mb_account_create(s, &ai, income));
  ASSERT_OK(mb_account_create(s, &ab, bank));

  char *r = NULL, cp[40], inv1[40], inv2[40], args[300];
  ASSERT_OK(mb_api_dispatch(s, "counterparty.create", "{\"name\":\"Acme\",\"kind\":\"CUSTOMER\"}", &r));
  grab(r, "id", cp, sizeof cp); free(r);

  /* invoice 1: $400, overpaid $1000 → fully PAID + $600 credit */
  snprintf(args, sizeof args, "{\"counterparty_id\":\"%s\"}", cp);
  ASSERT_OK(mb_api_dispatch(s, "invoice.create", args, &r)); grab(r, "id", inv1, sizeof inv1); free(r);
  snprintf(args, sizeof args,
    "{\"invoice_id\":\"%s\",\"description\":\"Work\",\"unit_price\":40000,\"account_id\":\"%s\"}", inv1, income);
  ASSERT_OK(mb_api_dispatch(s, "invoice.add_line", args, &r)); free(r);
  snprintf(args, sizeof args, "{\"invoice_id\":\"%s\",\"issue_date\":\"2026-08-01\"}", inv1);
  ASSERT_OK(mb_api_dispatch(s, "invoice.issue", args, &r)); free(r);
  snprintf(args, sizeof args,
    "{\"date\":\"2026-08-05\",\"amount\":100000,\"cash_account_id\":\"%s\",\"target\":\"INVOICE\",\"target_id\":\"%s\"}", bank, inv1);
  ASSERT_OK(mb_api_dispatch(s, "payment.record", args, &r)); free(r);

  /* counterparty.balance reports the credit through the JSON contract */
  snprintf(args, sizeof args, "{\"counterparty_id\":\"%s\",\"target\":\"INVOICE\"}", cp);
  ASSERT_OK(mb_api_dispatch(s, "counterparty.balance", args, &r));
  cJSON *j = cJSON_Parse(r); free(r);
  ASSERT_MONEY_EQ((mb_money)cJSON_GetObjectItem(j, "balance")->valuedouble, -60000);
  ASSERT_MONEY_EQ((mb_money)cJSON_GetObjectItem(j, "credit_available")->valuedouble, 60000);
  cJSON_Delete(j);

  /* invoice 2: $300 — apply $300 credit, no cash → PAID */
  snprintf(args, sizeof args, "{\"counterparty_id\":\"%s\"}", cp);
  ASSERT_OK(mb_api_dispatch(s, "invoice.create", args, &r)); grab(r, "id", inv2, sizeof inv2); free(r);
  snprintf(args, sizeof args,
    "{\"invoice_id\":\"%s\",\"description\":\"More\",\"unit_price\":30000,\"account_id\":\"%s\"}", inv2, income);
  ASSERT_OK(mb_api_dispatch(s, "invoice.add_line", args, &r)); free(r);
  snprintf(args, sizeof args, "{\"invoice_id\":\"%s\",\"issue_date\":\"2026-09-01\"}", inv2);
  ASSERT_OK(mb_api_dispatch(s, "invoice.issue", args, &r)); free(r);
  snprintf(args, sizeof args,
    "{\"date\":\"2026-09-02\",\"counterparty_id\":\"%s\",\"target\":\"INVOICE\",\"target_id\":\"%s\",\"amount\":30000}", cp, inv2);
  ASSERT_OK(mb_api_dispatch(s, "credit.apply", args, &r)); free(r);

  snprintf(args, sizeof args, "{\"id\":\"%s\"}", inv2);
  ASSERT_OK(mb_api_dispatch(s, "invoice.get", args, &r));
  char status[16]; grab(r, "status", status, sizeof status); free(r);
  ASSERT_STR_EQ(status, "PAID");

  /* over-applying remaining credit ($300 left) to a fresh $50 invoice is capped/rejected by remaining */
  snprintf(args, sizeof args, "{\"counterparty_id\":\"%s\",\"target\":\"INVOICE\"}", cp);
  ASSERT_OK(mb_api_dispatch(s, "counterparty.balance", args, &r));
  j = cJSON_Parse(r); free(r);
  ASSERT_MONEY_EQ((mb_money)cJSON_GetObjectItem(j, "credit_available")->valuedouble, 30000);
  cJSON_Delete(j);
  mb_store_close(s);
}

#endif
