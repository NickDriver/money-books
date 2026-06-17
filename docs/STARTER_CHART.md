# Starter Chart of Accounts — "US Freelancer / Software & Consulting"

> The default template offered by the first-run wizard (D7). **Everything is user-editable** —
> users rename, add, archive, or delete freely. Numbering follows the conventional ranges
> (Assets 1000s, Liabilities 2000s, Equity 3000s, Income 4000s, Expenses 6000s).
>
> `role`: `SYSTEM` (engine-required, not deletable), `ACCOUNT` (where money is — assets/liabilities),
> `CATEGORY` (what it's for — income/expenses). Maps to the data model in [SPEC.md](SPEC.md) §4.1.
> Not tax advice — Schedule C lines are approximate; confirm with a CPA.

## System accounts (always created; not deletable)

| Code | Name | Type | Role | Notes |
|---|---|---|---|---|
| 1200 | Accounts Receivable | Asset | SYSTEM | invoices post here |
| 2000 | Accounts Payable | Liability | SYSTEM | bills post here |
| 2200 | Sales Tax Payable | Liability | SYSTEM | optional tax line (D15) |
| 3900 | Opening Balance Equity | Equity | SYSTEM | offsets initial balances |

## Accounts — where money is

| Code | Name | Type | Role |
|---|---|---|---|
| 1000 | Business Checking | Asset | ACCOUNT |
| 1010 | Business Savings | Asset | ACCOUNT |
| 1020 | Cash on Hand | Asset | ACCOUNT |
| 1030 | Payment Processor (Stripe/PayPal) | Asset | ACCOUNT |
| 2100 | Business Credit Card | Liability | ACCOUNT |

## Owner's equity

| Code | Name | Type | Role | Notes |
|---|---|---|---|---|
| 3000 | Owner's Capital / Contributions | Equity | ACCOUNT | money put in |
| 3100 | Owner's Draw | Equity | ACCOUNT | money taken out (contra — normally a debit balance) |

*(Retained earnings is **derived** = cumulative Income − Expenses; no posting account, per SPEC §5.)*

## Categories — income (services)

| Code | Name | Type | Role |
|---|---|---|---|
| 4000 | Consulting Income | Income | CATEGORY |
| 4010 | Software Development Income | Income | CATEGORY |
| 4020 | Maintenance & Support Income | Income | CATEGORY |
| 4090 | Other Income | Income | CATEGORY |

## Categories — expenses (Schedule C-aligned)

| Code | Name | Type | Role | ~Schedule C |
|---|---|---|---|---|
| 6000 | Advertising & Marketing | Expense | CATEGORY | Line 8 |
| 6010 | Software & Subscriptions | Expense | CATEGORY | Line 27a |
| 6020 | Cloud Hosting & Infrastructure | Expense | CATEGORY | Line 27a |
| 6030 | Contractors / Subcontractors | Expense | CATEGORY | Line 11 |
| 6040 | Hardware & Equipment | Expense | CATEGORY | Line 13 / 22 |
| 6050 | Bank & Payment Processing Fees | Expense | CATEGORY | Line 27a |
| 6060 | Professional Services | Expense | CATEGORY | Line 17 |
| 6070 | Office Supplies | Expense | CATEGORY | Line 22 |
| 6080 | Home Office | Expense | CATEGORY | Line 30 (Form 8829) |
| 6090 | Internet & Phone | Expense | CATEGORY | Line 25 |
| 6100 | Travel | Expense | CATEGORY | Line 24a |
| 6110 | Meals (50% deductible) | Expense | CATEGORY | Line 24b |
| 6120 | Education & Training | Expense | CATEGORY | Line 27a |
| 6130 | Dues & Memberships | Expense | CATEGORY | Line 27a |
| 6140 | Business Insurance | Expense | CATEGORY | Line 15 |
| 6150 | Taxes & Licenses | Expense | CATEGORY | Line 23 |
| 6900 | Other Expenses | Expense | CATEGORY | Line 27a |

## Suggested default service items (D14)

Pre-seed a couple of editable service items so invoicing works immediately:

| Name | Default unit price | Unit | → Income category |
|---|---|---|---|
| Hourly Consulting | (user sets) | hour | 4000 Consulting Income |
| Software Development | (user sets) | hour | 4010 Software Development Income |

## Machine-readable seed (for the first-run wizard)

```json
{
  "template": "us_freelancer_software_consulting",
  "currency": "USD",
  "accounts": [
    {"code":"1000","name":"Business Checking","type":"ASSET","role":"ACCOUNT"},
    {"code":"1010","name":"Business Savings","type":"ASSET","role":"ACCOUNT"},
    {"code":"1020","name":"Cash on Hand","type":"ASSET","role":"ACCOUNT"},
    {"code":"1030","name":"Payment Processor (Stripe/PayPal)","type":"ASSET","role":"ACCOUNT"},
    {"code":"1200","name":"Accounts Receivable","type":"ASSET","role":"SYSTEM"},
    {"code":"2000","name":"Accounts Payable","type":"LIABILITY","role":"SYSTEM"},
    {"code":"2100","name":"Business Credit Card","type":"LIABILITY","role":"ACCOUNT"},
    {"code":"2200","name":"Sales Tax Payable","type":"LIABILITY","role":"SYSTEM"},
    {"code":"3000","name":"Owner's Capital / Contributions","type":"EQUITY","role":"ACCOUNT"},
    {"code":"3100","name":"Owner's Draw","type":"EQUITY","role":"ACCOUNT"},
    {"code":"3900","name":"Opening Balance Equity","type":"EQUITY","role":"SYSTEM"},
    {"code":"4000","name":"Consulting Income","type":"INCOME","role":"CATEGORY"},
    {"code":"4010","name":"Software Development Income","type":"INCOME","role":"CATEGORY"},
    {"code":"4020","name":"Maintenance & Support Income","type":"INCOME","role":"CATEGORY"},
    {"code":"4090","name":"Other Income","type":"INCOME","role":"CATEGORY"},
    {"code":"6000","name":"Advertising & Marketing","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6010","name":"Software & Subscriptions","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6020","name":"Cloud Hosting & Infrastructure","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6030","name":"Contractors / Subcontractors","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6040","name":"Hardware & Equipment","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6050","name":"Bank & Payment Processing Fees","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6060","name":"Professional Services","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6070","name":"Office Supplies","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6080","name":"Home Office","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6090","name":"Internet & Phone","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6100","name":"Travel","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6110","name":"Meals (50% deductible)","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6120","name":"Education & Training","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6130","name":"Dues & Memberships","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6140","name":"Business Insurance","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6150","name":"Taxes & Licenses","type":"EXPENSE","role":"CATEGORY"},
    {"code":"6900","name":"Other Expenses","type":"EXPENSE","role":"CATEGORY"}
  ],
  "items": [
    {"kind":"SERVICE","name":"Hourly Consulting","unit_label":"hour","income_code":"4000"},
    {"kind":"SERVICE","name":"Software Development","unit_label":"hour","income_code":"4010"}
  ]
}
```

> The "start empty" wizard option still creates the four **SYSTEM** accounts (1200/2000/2200/3900),
> since the accrual engine requires AR/AP/tax/opening-balance accounts to function.
