#include "seed.h"

#include <stdlib.h>
#include <string.h>
#include "../account/account.h"

/* Canonical starter chart — mirrors docs/STARTER_CHART.md. */
typedef struct {
  const char     *code;
  const char     *name;
  mb_account_type type;
  mb_account_role role;
} seed_acct;

static const seed_acct CHART[] = {
  /* assets */
  {"1000", "Business Checking",                 MB_ACCT_ASSET,     MB_ROLE_ACCOUNT},
  {"1010", "Business Savings",                  MB_ACCT_ASSET,     MB_ROLE_ACCOUNT},
  {"1020", "Cash on Hand",                      MB_ACCT_ASSET,     MB_ROLE_ACCOUNT},
  {"1030", "Payment Processor (Stripe/PayPal)", MB_ACCT_ASSET,     MB_ROLE_ACCOUNT},
  {"1200", "Accounts Receivable",               MB_ACCT_ASSET,     MB_ROLE_SYSTEM},
  /* liabilities */
  {"2000", "Accounts Payable",                  MB_ACCT_LIABILITY, MB_ROLE_SYSTEM},
  {"2100", "Business Credit Card",              MB_ACCT_LIABILITY, MB_ROLE_ACCOUNT},
  {"2200", "Sales Tax Payable",                 MB_ACCT_LIABILITY, MB_ROLE_SYSTEM},
  /* equity */
  {"3000", "Owner's Capital / Contributions",   MB_ACCT_EQUITY,    MB_ROLE_ACCOUNT},
  {"3100", "Owner's Draw",                      MB_ACCT_EQUITY,    MB_ROLE_ACCOUNT},
  {"3900", "Opening Balance Equity",            MB_ACCT_EQUITY,    MB_ROLE_SYSTEM},
  /* income */
  {"4000", "Consulting Income",                 MB_ACCT_INCOME,    MB_ROLE_CATEGORY},
  {"4010", "Software Development Income",        MB_ACCT_INCOME,    MB_ROLE_CATEGORY},
  {"4020", "Maintenance & Support Income",      MB_ACCT_INCOME,    MB_ROLE_CATEGORY},
  {"4090", "Other Income",                      MB_ACCT_INCOME,    MB_ROLE_CATEGORY},
  /* expenses (Schedule C-aligned) */
  {"6000", "Advertising & Marketing",           MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6010", "Software & Subscriptions",          MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6020", "Cloud Hosting & Infrastructure",    MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6030", "Contractors / Subcontractors",      MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6040", "Hardware & Equipment",              MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6050", "Bank & Payment Processing Fees",    MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6060", "Professional Services",             MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6070", "Office Supplies",                   MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6080", "Home Office",                       MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6090", "Internet & Phone",                  MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6100", "Travel",                            MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6110", "Meals (50% deductible)",            MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6120", "Education & Training",              MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6130", "Dues & Memberships",                MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6140", "Business Insurance",                MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6150", "Taxes & Licenses",                  MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
  {"6900", "Other Expenses",                    MB_ACCT_EXPENSE,   MB_ROLE_CATEGORY},
};
static const int CHART_COUNT = (int)(sizeof CHART / sizeof CHART[0]);

/* Record the system account ids the engine looks up by role (AR/AP/Tax/Opening). */
static mb_err record_system_meta(mb_store *s, const char *code, const char *id) {
  const char *key = NULL;
  if (!strcmp(code, "1200")) key = "ar_account_id";
  else if (!strcmp(code, "2000")) key = "ap_account_id";
  else if (!strcmp(code, "2200")) key = "tax_account_id";
  else if (!strcmp(code, "3900")) key = "opening_balance_equity_id";
  if (key) MB_TRY(mb_store_meta_set(s, key, id));
  return MB_OK;
}

static mb_err seed(mb_store *s, int system_only) {
  for (int i = 0; i < CHART_COUNT; i++) {
    if (system_only && CHART[i].role != MB_ROLE_SYSTEM) continue;
    mb_account_new in = {.code = CHART[i].code, .name = CHART[i].name,
                         .type = CHART[i].type, .role = CHART[i].role};
    char id[40];
    MB_TRY(mb_account_create(s, &in, id));
    if (CHART[i].role == MB_ROLE_SYSTEM) MB_TRY(record_system_meta(s, CHART[i].code, id));
  }
  return MB_OK;
}

mb_err mb_seed_system_accounts(mb_store *s) { return seed(s, 1); }
mb_err mb_seed_starter_chart(mb_store *s)   { return seed(s, 0); }

#ifdef MB_TEST
#include "../support/mb_test.h"

TEST(seed, system_only_creates_four) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_system_accounts(s));
  int n = 0; ASSERT_OK(mb_account_count(s, &n));
  ASSERT_EQ_INT(n, 4);
  mb_account a;
  ASSERT_OK(mb_account_find_by_code(s, "1200", &a));  /* Accounts Receivable */
  ASSERT_EQ_INT(a.role, MB_ROLE_SYSTEM);
  mb_store_close(s);
}

TEST(seed, starter_chart_full) {
  mb_store *s = NULL; ASSERT_OK(mb_store_open_memory(&s));
  ASSERT_OK(mb_seed_starter_chart(s));
  int n = 0; ASSERT_OK(mb_account_count(s, &n));
  ASSERT_EQ_INT(n, 32);  /* full template */
  /* a known income category and expense category exist */
  mb_account a;
  ASSERT_OK(mb_account_find_by_code(s, "4000", &a)); ASSERT_EQ_INT(a.type, MB_ACCT_INCOME);
  ASSERT_OK(mb_account_find_by_code(s, "6010", &a)); ASSERT_EQ_INT(a.type, MB_ACCT_EXPENSE);
  /* listing categories returns the income+expense set */
  mb_account *list = NULL; int c = 0;
  mb_account_filter f = {.has_role = 1, .role = MB_ROLE_CATEGORY};
  ASSERT_OK(mb_account_list(s, &f, &list, &c));
  ASSERT_EQ_INT(c, 21);  /* 4 income + 17 expense */
  free(list);
  mb_store_close(s);
}
#endif
