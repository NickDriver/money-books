# Money Books — Test Suite Audit

> Critical review of the existing test suite. **No source or test was modified.** This is a
> triage document: each finding is a hypothesis to investigate one by one, framed as
> "if the code had a bug here, would this test catch it?" Per the owner's philosophy, nothing
> here proposes "adjusting tests to pass" — where a test looks weak because the *code* may be
> wrong, that is called out as a real-bug hypothesis to chase in the code, not in the test.

## 1. Summary

- **Tests reviewed:** 76 (all green: `make test` → `76 run, 76 passed, 0 failed`), across 25 files.
- **Headline verdict:** The suite is **above average for a hand-rolled C harness**. The core
  double-entry invariants (Σ postings = 0, no-commit-on-failure, reversal nets to zero, immutable
  journal, hash chain) are tested with **independently derived arithmetic**, not copied magic
  values. Money/journal/account/report/invoice/mcp/agent tests are genuinely strong. The
  weaknesses are concentrated in **(a) one mislabeled negative test that asserts the wrong thing,
  (b) a real missing guard (payment overpayment), (c) the known bill/AP coverage asymmetry, and
  (d) a handful of "round-trip / happy-path only" tests that would survive a real bug.**
- **Findings by severity:** HIGH **3** · MED **6** · LOW **4**. Plus a Coverage-Gap section
  (behaviors with *no* test).

The single most important takeaway: **finding F1 (overpayment not rejected) is a likely real bug
that no test guards**, and the test that *looks* like it should (`payment.rejects_non_open_and_bad_amount`)
does not actually exercise a bad amount at all (F2).

---

## 2. Findings

Severity: **HIGH** = masks a likely real bug or asserts nothing meaningful · **MED** = weak but
not dangerous · **LOW** = cosmetic / stylistic.

> **RESOLUTION (owner, 2026-06-17):** This is NOT a bug to guard — overpayment is an **intended
> feature**. Customers may pay more than an invoice (and we may overpay a vendor), creating a
> **credit balance tracked per-counterparty via AR/AP** (chosen model: **AR/AP balance-forward** —
> the counterparty's net AR/AP balance IS their credit — with **manual** application of available
> credit to specific open documents). The current code "silently accepts overpay but tracks no
> per-counterparty balance and hides the overpaid doc from aging" is the wrong middle state; the
> fix is to BUILD the credit feature (v5 migration tagging AR/AP postings with counterparty_id,
> a `credit.apply` op, per-counterparty statement/balance, UI, and tests), not to add a reject
> guard. Tracked as a feature (proposed decision D26), not a code bug. Tests still needed: overpay
> → credit recorded → balance net-negative → manual apply to a new doc → status + Σ=0 hold.

### F1 — Overpayment / overpay-to-PAID is never tested (and the code does not guard it)
- **file:line:** `src/payment/payment.c:199` (`payment.rejects_non_open_and_bad_amount`), gap against `src/payment/payment.c:57` (`mb_payment_record`)
- **Should verify:** that a payment cannot exceed the open balance of an invoice/bill, or — if
  overpayment is intentionally allowed — that the resulting state (AR sign, status) is the
  documented one.
- **Concern:** `mb_payment_record` validates only `amount <= 0` (line 61) and the target status
  (line 68). There is **no upper-bound check** against remaining balance. Status is recomputed as
  `paid >= total ? "PAID" : "PARTIAL"` (line 134). So recording a $2000 payment on a $1000 invoice
  is accepted, marks the invoice **PAID**, and drives **Accounts Receivable to −$1000** (a credit
  balance on a debit-normal control account). No test covers this; `invoice_partial_then_paid` only
  ever pays exact amounts.
- **Severity:** **HIGH**
- **Real-bug hypothesis?** **Yes.** Confirmed by reading the code path: no clamp/reject on
  overpayment exists, and AR can be pushed negative while the doc reads PAID. Whether that is a bug
  or an accepted v1 simplification is a product call — but it is currently **untested either way**,
  so the books can silently go into a state the spec's "always balanced / audit-true" principle
  arguably forbids. Investigate first.

### F2 — `payment.rejects_non_open_and_bad_amount` tests neither a bad amount nor the AP side
- **file:line:** `src/payment/payment.c:199-211`
- **Should verify (per its name):** (a) payment against a non-payable (DRAFT/PAID/VOID) target is
  rejected, AND (b) a bad amount (zero / negative / over-balance) is rejected.
- **Concern:** The body issues exactly one call: `mb_payment_record(..., 100, ..., inv, ...)` against
  a **DRAFT** invoice, expecting `MB_ERR_INVALID_ARG`. That error fires from the **status check**
  (line 68), not from any amount check — `100` is a perfectly valid positive amount. So the "bad
  amount" half of the name is **never exercised**: `amount <= 0` (line 61) has zero coverage, and
  there is no over-balance case (ties to F1). The test also never touches `MB_PAY_BILL`.
- **Severity:** **HIGH** (name promises a negative-input contract the body does not test; combined
  with F1 it means the amount-validation branch is effectively untested).
- **Real-bug hypothesis?** No bug in the `<=0` branch itself (it exists and looks correct), but the
  branch is **uncovered**, so a future regression that drops it would not be caught.

### F3 — Bill module: D13 lock and the entire edit/revert family are untested (invoice/bill asymmetry)
- **file:line:** `src/bill/bill.c:375` and `:403` are the only two bill tests; gap against
  `mb_bill_enter` lock, `mb_bill_lines` (`:271`), `mb_bill_remove_line` (`:310`),
  `mb_bill_update` (`:331`), `mb_bill_revert_to_draft` (`:349`).
- **Should verify:** the bill side should mirror the seven invoice tests — at minimum
  "cannot edit after enter" (the AP twin of `invoice.cannot_edit_after_issue`), "enter empty fails",
  and a reopen→reverse→re-enter round trip proving AP nets to zero (the twin of
  `invoice.edit_draft_lines_and_reopen_open`).
- **Concern:** Bills have only `enter_posts_expense_and_ap` and `list_carries_vendor_status_total`.
  The invoice mirror functions are well tested; the bill mirror functions have **no unit test at
  all**, despite being a copy-paste-symmetric code path where a divergence (wrong control account,
  missing lock, reversal not posted) would be invisible.
- **Severity:** **MED** (it is "missing coverage" rather than a "weak test", but it is a
  deliberate-looking asymmetry on a money-moving path, so it ranks above cosmetic).
- **Real-bug hypothesis?** Unknown — that is exactly the risk. The reopen-reverse logic in
  particular (AP must net to zero on revert) has no proof it works on the bill side.

### F4 — `redact.prefix_collision_safe` proves a round-trip but not that redaction happened
- **file:line:** `src/redact/redact.c:165-183`
- **Should verify:** that 11+ counterparties tokenize without `Client_1` clobbering `Client_10`,
  **and** that names were actually replaced (not a no-op) and mapped to the *correct* originals.
- **Concern:** The only assertion is `ASSERT_STR_EQ(back, txt)` after `red = mb_redact(...)` then
  `back = mb_restore(red)`. If `mb_redact` and `mb_restore` were both the identity function (a real
  failure mode for a redactor), this test would still pass. It never asserts the redacted middle
  state (`red`) lacks the original names or contains the tokens. Contrast with the *good* sibling
  `redact.names_to_tokens_and_back` (`:134`) which does assert `strstr(red,"Acme Corp")==NULL` and
  `strstr(red,"Client_1")!=NULL`, and `agent.redacts_before_sending` (`:181`) which checks egress.
- **Severity:** **MED**
- **Real-bug hypothesis?** No (the sibling test shows redaction works); but this specific test would
  not catch a regression where the longest-key-first ordering breaks and tokens are left unredacted,
  *as long as restore symmetrically failed too* — which is plausible since both share the map.

### F5 — Report layer never includes a REVERSED entry, so "POSTED-only" filtering is unproven there
- **file:line:** `src/report/report.c` — all of `trial_balance_balances` (`:350`),
  `pnl_and_balance_sheet` (`:365`), `category_txns_income_and_expense` (`:480`),
  `journal_lists_all_entries` (`:461`).
- **Should verify (per SPEC §6.1):** `category_transactions` shows **only effective (POSTED)**
  entries (excludes reversals), while `transactions`/journal **includes reversals**; and a reversed
  entry must not double-count in TB/P&L/Balance Sheet.
- **Concern:** Every report test posts only fresh `POSTED` entries. None posts an entry, reverses
  it, and then asserts (a) the journal view shows 3 rows incl. the REVERSAL, (b) `category_txns`
  excludes the reversed pair, (c) P&L/TB net out. The integration test `engine_flow` does reverse
  and re-check *account balances*, but the **report SQL's status handling is never exercised**.
  A bug where `category_txns` forgot its `status='POSTED'` filter (or TB summed reversals twice)
  would pass the whole suite.
- **Severity:** **MED**
- **Real-bug hypothesis?** Possible — unverified. The status-filtering SQL in the report layer is a
  distinct code path from `mb_account_balance`, and it has no negative/reversal fixture.

### F6 — `report.ar_aging_buckets` tests one bucket; other buckets and `ap_aging` are untested
- **file:line:** `src/report/report.c:424` (and total gap: `mb_report_ap_aging` at
  `src/report/report.c:207` has **zero** tests).
- **Should verify:** all aging buckets (current / 1-30 / 31-60 / 61-90 / 90+) land correctly given
  due dates, on both AR and AP.
- **Concern:** The test issues one invoice due 2026-01-31, checks as-of 2026-02-10, and asserts only
  `total`, `d1_30`, and `current==0`. The boundary arithmetic (what falls in 31-60 vs 61-90 vs 90+,
  and the `current` vs just-overdue cutoff) is never tested, so an off-by-one in the `julianday`
  bucket boundaries would not be caught. `ap_aging` is entirely uncovered.
- **Severity:** **MED**
- **Real-bug hypothesis?** Possible — date-bucket boundary math is a classic off-by-one site and is
  unexercised at the edges.

### F7 — `api.invoice_payment_flow` asserts only final status, not the resulting balances
- **file:line:** `src/api/api.c:992-1025`
- **Should verify:** the full create→add_line→issue→pay path through the JSON API produces a PAID
  invoice **and** the correct AR/bank ledger balances.
- **Concern:** After paying the full $1000 it asserts only `status == "PAID"`. It never re-reads the
  AR or bank balance through the API, so a bug where the API-layer payment posts the wrong amount or
  to the wrong account (but still flips status because `paid >= total`) would pass. The engine-layer
  `payment.invoice_partial_then_paid` does check balances, so this is a thin API-layer assertion
  rather than a hole in the engine.
- **Severity:** **MED**
- **Real-bug hypothesis?** No (engine path is covered); flagged because the API translation layer
  itself (arg names → engine call) is asserted only by a boolean status.

### F8 — `store.stamps_increment` uses EXPECT (non-fatal) for the core monotonic-stamp invariant
- **file:line:** `src/store/store.c:303-314`
- **Concern:** The Lamport/seq monotonicity (a D20 sync-identity invariant) is checked with
  `EXPECT(s2 == s1+1)` rather than `ASSERT`. `EXPECT` records a failure but continues, which is fine
  for reporting — but it signals this is treated as soft. Minor.
- **Severity:** **LOW**
- **Real-bug hypothesis?** No.

### F9 — `integration.invoice_total` name/behavior mismatch
- **file:line:** `tests/integration_smoke.c:10`
- **Concern:** Named `invoice_total` and lives in the "integration" suite, but the body only parses
  three money strings, sums them, and formats — **no invoice, no store, no cross-module flow.** It
  is a `money`-module unit test wearing an integration name. The file's own header comment admits it
  ("for now this composes the money primitive"). Harmless but misleading for triage.
- **Severity:** **LOW**
- **Real-bug hypothesis?** No.

### F10 — `seed.system_only_creates_four` does not verify the recorded control-account meta keys
- **file:line:** `src/seed/seed.c:85`
- **Concern:** Seeding records `ar_account_id` / `ap_account_id` / `tax_account_id` / opening into
  `book_meta` (relied on by invoice/bill/payment). This test asserts the 4 accounts exist and AR has
  role SYSTEM, but not that the meta keys were written. Those keys are exercised indirectly by other
  modules' tests, so coverage exists — but the seed test itself under-asserts its own contract.
- **Severity:** **LOW**
- **Real-bug hypothesis?** No (covered transitively).

### F11 — `api.onboarding_seeds_once` asserts `account_count > 0`, not the expected count
- **file:line:** `src/api/api.c:886` (esp. `:904` `ASSERT(after > 0)`)
- **Concern:** After onboarding with the freelancer template it asserts only `after > 0` and that a
  re-onboard leaves the count unchanged. The "seeds once / idempotent" claim is well tested, but the
  "seeds the *right* chart" claim is reduced to ">0". `seed.starter_chart_full` pins the real count
  (32) at the engine layer, so this is acceptable; noting it as a deliberately loose API-layer check.
- **Severity:** **LOW**
- **Real-bug hypothesis?** No.

---

## 3. Coverage Gaps (behaviors with NO test — distinct from weak tests above)

These confirm and frame the pre-measured gaps; not re-measured exhaustively.

- **`api.c` JSON contract (~57% line cov).** ~30 `mb_api_dispatch` handlers never executed by any
  test: **all `bill.*`**, `invoice.edit`/`list`/`revert`/`remove_line`, **all reports except P&L**
  (no `report.trial_balance`/`balance_sheet`/`cash_flow`/`journal`/`category_txns`/`ledger` via the
  API), `account.list`/`update`/`set_active`, `item.*`, `counterparty.list`,
  `transaction.post`, `agent.send`. The API is the shared UI+MCP contract, so these are the
  arg-mapping/JSON-shape paths most likely to drift unseen.
- **`bill.c` edit/detail family (~64%).** See F3 — `mb_bill_lines`, `mb_bill_update`,
  `mb_bill_remove_line`, `mb_bill_revert_to_draft`, `mb_bill_status_str`, and the post-`enter` lock:
  **no unit test**, in contrast to their invoice twins.
- **`mb_report_ap_aging`** (`src/report/report.c:207`): **never tested at all** (F6).
- **`llm.c` (0%).** The pure translation functions (`to_openai_messages` / `to_openai_tools` /
  `from_openai`) are deterministic and unit-testable without network/keys, but have **no test**.
  The agent loop is tested only via a mock provider, so the real OpenAI dialect serialization is
  unproven.
- **`src/app/main.c` and `src/mcpd/main.c`:** no tests. The multi-company shell logic
  (`shell_dispatch` / `app.*` book open/create/switch / `derive_path` / "no book open" guard) is
  untested; `registry`/`book` units cover the pieces but not the shell that wires them.
- **`mb_error.c` (~45%).** Error-formatting / breadcrumb (`MB_TRY` trail) / last-error paths
  partially covered as a side effect of assertions; no dedicated test.
- **Payment — bill side (`MB_PAY_BILL`).** No test records a bill payment (Dr AP / Cr Cash) or
  drives a bill PARTIAL→PAID. (Subset of F1/F3.)
- **Item account-type matching (SPEC §4.2).** `mb_item_create` (`src/item/item.c:12`) checks only
  FK existence — it does **not** enforce SERVICE→INCOME / EXPENSE→EXPENSE account linkage that the
  spec describes. No test asserts either the enforcement or its (current) absence. Treat as a
  spec-vs-code question to confirm with the owner, not necessarily a bug.
- **Invoice/bill `VOID` path.** `void_invoice` is in the spec tool surface; no test posts a void or
  asserts a voided doc is locked from payment/edit.
- **Overflow / extreme inputs at the document layer.** `money` overflow is well tested in isolation,
  but no invoice/bill with a line total near `INT64_MAX` exercises the overflow propagation through
  `mb_invoice_total` / posting.

---

## 4. Prioritized Recommendation (investigate in this order)

1. **F1 — overpayment guard.** Highest value: a money-correctness question on a write path, with a
   confirmed missing check and no test. Decide product intent (reject vs allow), then either add the
   guard (code) or document the allowed state — and add the corresponding test.
2. **F2 — fix the misleading negative test's *intent*.** The `<=0` and over-balance branches are
   uncovered; once F1 is resolved, this test should genuinely exercise a bad amount on an OPEN
   invoice (do not weaken it to match current behavior).
3. **F3 — bill edit/revert + lock coverage.** Mirror the invoice tests; the reopen→reverse "AP nets
   to zero" path is money-moving and entirely unproven.
4. **F5 — reversal in the report layer.** Add a reversed-entry fixture and assert journal-includes /
   category_txns-excludes / TB-and-P&L-net-out. This is the most likely place a silent
   status-filter bug hides.
5. **F6 — aging buckets + ap_aging.** Cheap to add, classic off-by-one risk at boundaries.
6. **F4, F7** — strengthen assertions (redaction must prove the redacted middle state; API payment
   should re-read balances). Low effort, removes "round-trip / status-only" blind spots.
7. **Coverage gaps** — prioritize the **api.c bill/report handlers** (shared UI+MCP contract) and
   the **llm.c translation functions** (deterministic, network-free, currently 0%).

> Note on what is *good*, to avoid over-correcting: `money.*`, `journal.*` (balanced/unbalanced/
> missing-account/reverse/hash-chain), `account.*`, the `report` arithmetic (independently derived,
> date-windowed, running-balance with opening seed), `invoice.*` (D13 reopen-reverse net-zero,
> cannot-edit-after-issue, empty-issue), `mcp.blocked_tool_hidden_and_refused`, `agent.*`,
> `settings_provider_key_flow` (key never leaks), and `engine_flow` (global Σ=0 after reversal) are
> all genuine specifications of intended behavior and would catch real regressions. The journal is
> immutable **by construction** (no edit/delete function exists — only post + reverse), which is the
> strongest possible form of the D13 guarantee.
