# Money Books — Project Notes & Decision Log

> Living document for the preparation phase. We chat → record → research → write spec.
> No coding yet. One question at a time. If a session restarts, resume from "Open Questions Queue".

Last updated: 2026-06-12

---

## 1. Vision (one-liner)

A simple, AI-native, local-first double-entry accounting app for Mac (Apple Silicon),
written in pure C with a web-like native UI, an integrated MCP server so an LLM can
read/manipulate the books, and a side-bar AI agent chat. Few buttons, clean dashboard,
proper bookkeeping fundamentals.

---

## 2. Captured Requirements (from user, verbatim intent)

### Tech context
- **Language:** pure C.
- **UI:** "web-like" interface but **native**. Target **macOS / Apple Silicon (M chip)** first.
  - Open question whether to use Apple libraries, an Electron/Tauri-style shell "but for C",
    or build our own web-based UI library.
- **Process:** research first → spec-driven development → spec as a markdown doc.
- **Testing:** assertions, per-file unit tests (Rust-style, tests live next to code),
  integration tests in a separate folder (Rust-style).
- **Storage:** local-first. Likely **SQLite** to start (wants my opinion).
- **Future:** P2P sync to stay local-first — candidate lib **iroh**. Possibly a **VPS**
  for metadata + sync assistance.

### Functional context
- Double-entry journal (industry standard).
- Dashboard, **few buttons**.
- **Integrated MCP server** so Claude / other LLMs can connect and manipulate data.
- **AI agent chat** in a side bar — via MCP / an MCP engine, or directly (it's a local app,
  not a standard web app with API endpoints).
- Simple but with bookkeeping minimums:
  - **Accounts** and **categories** — NOT hardcoded; user can add their own.
  - Basic **reporting**: Balance Sheet, P&L, plus **filtration**.
  - Adjustable **dictionaries** for services provided and expenses tracked.
- Goal: basic accounting software, but designed to be manipulated by an AI agent.

---

## 3. My Recommendations / Opinions (to confirm)

- **Storage → SQLite: yes, strongly agree.** Single-file, transactional (ACID — critical for
  double-entry), zero-config, ubiquitous, great C API. It also gives us a clean future bridge:
  pair it with an append-only event/journal model now so P2P sync (iroh + CRDT-ish merge) is
  feasible later without a rewrite. (Detail TBD.)
- **Native web UI → use the system WebView (WKWebView / WebKit), not bundled Chromium.**
  This is essentially "Tauri for C": macOS ships WebKit, so we render HTML/CSS/JS in a
  WKWebView and drive it from a C core. No 100MB+ Chromium bundle, genuinely native, tiny.
  C talks to WKWebView through a thin Objective-C shim (Apple's Obj-C runtime is callable from C).
  (To be confirmed as a decision — see Q1.)

---

## 4. Decision Log (confirmed)

> **▶ RESUME HERE (last session 2026-06-19).** **Phases 0–4 COMPLETE + multi-company (D25) + customer/
> vendor credit (D26) + document lifecycle tightened; Phase 5 = MCP server only (in-app agent removed),
> now hardened.** `make test` = **87/87 green, 0 leaks**; `make app` + `make mcp` + UI build clean;
> published at **github.com/NickDriver/money-books** (public, MIT). The React UI is wired (Dashboard,
> Record, Transactions, Invoices&Bills w/ detail+edit+**Apply credit**+**Void**, Accounts&Categories w/
> editing+ledger, Items w/ **All/Service/Expense filter**, Reports, company launcher w/ per-company
> **Connect-to-Claude** + **MCP tools window**). Latest build details are in the dated BUILD STATUS
> blocks below (newest first).
>
> **Test-suite audit (docs/TEST_AUDIT.md) — COMPLETE.** All findings F1–F11 closed; the one real bug
> it surfaced (Concern C1 — reversed entries not netted out of `category_txns`) is **fixed**. As part
> of that, the invoice/bill **lifecycle was tightened**: issued documents are immutable (no more
> reopen-to-draft); corrections are made with new documents or by **Void**. No open audit findings.
>
> **Phase 5 — AI = MCP server only.** The in-app sidebar agent was **removed** (see BUILD STATUS
> 2026-06-19 below): AI access is now solely through the stdio MCP server (the user brings their own
> LLM client, e.g. Claude Desktop). The engine makes no outbound network calls.
>
> **NEXT — ▶ Phase 7 (P2P sync), re-sequenced BEFORE Phase 6 (user decision 2026-06-19).** The big
> one: `iroh-c-ffi` QUIC transport (dial-by-public-key + NAT traversal + relay), **version-vector
> sync** of the append-only journal (no CRDT — entries are immutable, D20 identity already in place),
> optional VPS assist. Introduces the **Rust toolchain** (first non-C dep). See SPEC §13.
>
> **Cross-platform (D27, decided 2026-06-19): one codebase, compile-time `#ifdef` selection behind the
> existing thin seams (paths=`src/registry`, native shell=`src/app`, build), NOT separate forks.** Only
> build/packaging/signing is per-OS; the source is shared. The Mac `Makefile` is the one Mac-specific
> artifact → moves to **CMake** in Phase 6. Linux=XDG paths + GTK shell; Windows=%APPDATA% + Win32.
>
> **Deferred to Phase 6 (after Phase 7):** packaging (`.app`/MSI/AppImage, signing/notarization,
> NSOpenPanel "open book", CMake cross-platform build, CI). **Phase-5 fast-follows (anytime):**
> per-tool-policy settings screen; expose items/dashboard/journal/search as MCP tools (each w/ a
> `tests/mcp_tools.c` case); top-level entry for the MCP windows. Open §15 items: attachments,
> backup/export UX.
>
> **BUILD STATUS (2026-06-19): MCP hardened + discoverable — 87 tests green, 0 leaks.** Built on the
> agent removal below. (1) **Per-company Connect-to-Claude modal** (`ui/Launcher.jsx` + `app.mcp_info`
> shell method resolving real binary/book/Claude-config paths via `_NSGetExecutablePath`) generates a
> ready-to-paste MCP config snippet — one server entry per book. (2) **MCP tools window** lists the
> exposed surface (Read/Write) from `mb_mcp_tools_catalog` over the live `TOOLS[]` registry (new
> `mcp.tools` API method). (3) **Server-side write-approval gate**: every write needs `confirm:true`,
> else the server returns `approval_required` and writes nothing — independent of Permit/Ask/Block,
> works even if the client auto-approves. (4) Added `list_invoices`/`list_bills`/`get_bill` (now **23
> tools**). (5) **Integration tests** `tests/mcp_tools.c` exercise every tool over JSON-RPC — which
> caught a real bug: `post_transaction` (`h_transaction_post`) `malloc`'d `mb_posting_in` and left
> `counterparty_id` uninitialized (UB; ASan 0xbe → crash) — **fixed**. (6) UI: Items **All/Service/
> Expense** filter. Rule going forward (saved to memory): **every new MCP tool ships with a
> `tests/mcp_tools.c` case.** Docs updated (SPEC §8.2/§8.3/§14/§7, MCP.md).
>
> **BUILD STATUS (2026-06-19): in-app sidebar agent REMOVED — 84 tests green, 0 leaks.** Deleted the
> in-app agent feature: `src/agent` (tool-use loop), `src/llm` (OpenAI/OpenRouter via libcurl),
> `src/redact` (egress pseudonymization), and `src/secret` (Keychain — only held LLM keys, now dead).
> Dropped API methods `agent.send` + `settings.{list_providers,set_provider,clear_key}` from
> `src/api/api.c` (kept `app_setting_get/set`, still used by onboarding); removed the pthread agent
> worker from `src/app/main.c`; deleted UI `Chat.jsx` + `Settings.jsx` (Assistant/Settings tabs) and
> their `api.js` mocks. Makefile dropped `-lcurl`, the Keychain frameworks (`Security`/`CoreFoundation`)
> from `app`/`mcp`, and `-DMB_SECRET_MEMORY`. **MCP server (`src/mcp`, `src/mcpd`) untouched** — it has
> zero dependency on the agent. Docs updated (SPEC §8.4/§9, MCP.md, deleted AGENT.md). Rationale: the
> in-app agent duplicated what an external MCP client does better, and made the offline engine a
> network client. Net: −6 tests (2 agent, 2 redact, 1 secret, 1 settings-flow).
>
> **BUILD STATUS (2026-06-17): document lifecycle tightened + bug C1 fixed — 90 tests green, 0 leaks.**
> Refines D13: an issued invoice/bill is now **immutable** — editable only while DRAFT, locked on
> issue/enter (no more reopen-to-draft). Corrections are made with new documents or by **voiding**.
> Removed `mb_invoice_revert_to_draft`/`mb_bill_revert_to_draft` (+ `invoice.revert`/`bill.revert` API
> + the UI "Reopen to edit"). Added `mb_invoice_void`/`mb_bill_void` (+ `invoice.void`/`bill.void` API
> + a "Void" action with inline confirm on issued-unpaid docs): voiding an **OPEN** doc posts a
> reversing entry (cancels AR/AP + income/expense), marks the document `VOID`, and drops it from aging;
> rejected for DRAFT (never issued) and PARTIAL/PAID (cash applied — use refund / credit note / D26
> credit). **Fixed audit Concern C1:** `mb_journal_reverse` now flips the original entry to
> `status='REVERSED'` so status-filtered effective reports (`category_txns`) net reversed/voided
> entries out; postings stay immutable and the flag is outside the content hash, so the tamper-evident
> chain is unaffected; balance reports were already correct (they sum all postings). Tests reworked:
> `invoice.edit_draft_then_lock_on_issue` + `invoice.void_reverses_and_locks`, `bill.edit_draft_then_
> lock_and_void`, and `report.reversed_entry_nets_out` netting assertion re-enabled. **Audit triage
> F1–F11 + C1 all closed; no open findings.**
>
> **BUILD STATUS (2026-06-17): customer/vendor credit (D26) — 82 tests green, 0 leaks.** Resolves
> audit finding F1 (overpayment was silently accepted but untracked). **Design: AR/AP balance-forward
> + manual application.** Schema **migration v5**: `posting.counterparty_id` (tags AR/AP control
> postings → per-party balance), `payment.counterparty_id` (denormalized), and a new **`allocation`**
> open-item ledger `{counterparty_id, source_kind PAYMENT|CREDIT, payment_id?, target, amount}`.
> Engine: `mb_posting_in` gained `counterparty_id` (persisted in `post_core`, **preserved through
> reversals**); invoice-issue/bill-enter tag the AR/AP leg with the counterparty; `mb_payment_record`
> now allocates min(cash, remaining) and lets the **overage become available credit** (status
> recomputed from Σ allocations, not Σ payments — backward-compatible); new `mb_counterparty_balance`,
> `mb_credit_available` (Σ payments − Σ allocations, clamped ≥0), `mb_credit_apply` (manual; posts NO
> journal entry; validates payable/amount>0/≤remaining/≤available/same-counterparty). Reports: aging
> outstanding now = total − Σ allocations (so credit-settled docs aren't overstated). API: `credit.apply`
> + `counterparty.balance` (returns balance + credit_available). UI: invoice/bill detail shows
> "{Customer/Vendor} has $X credit available" and an **Apply credit** action + form (verified live in
> preview). Invariants proven by tests: `AR(cp)=docs_outstanding−available_credit`, Σ postings=0 after
> credit ops, credit is per-counterparty (one party can't spend another's), symmetric AP/vendor side.
> Tests added: `payment.overpay_records_customer_credit`, `…customer_credit_applies_to_new_invoice_manually`,
> `…credit_apply_validations`, `…credit_is_per_counterparty`, `…vendor_overpay_and_credit_apply`,
> `api.credit_overpay_and_apply_flow`. NOTE: the entry hash chain still covers (account, amount, memo)
> only — `counterparty_id` is an annotation on the AR/AP leg, not part of the tamper-evident content.
>
> **SPEC STATUS (2026-06-14): `docs/SPEC.md` reviewed §1–§16 and APPROVED.** During review added
> D22 (portable secret store), D23 (portability principle), D24 (React+Vite front-end), and confirmed:
> signed-amount postings with Dr/Cr display, invoice DRAFT→issue flow, derived retained earnings
> (no formal closing in v1).
>
> **BUILD STATUS (2026-06-14): Phase 1 engine COMPLETE — 27 tests green (ASan+UBSan), 0 leaks.**
> Built: `src/store` (open/WAL/migrations v1/meta/sync-stamps), `src/support/mb_id` (UUIDv4),
> `src/support/sha256` (hash chain), `src/account` (chart of accounts incl. `find_by_code` +
> filtered `list`), `src/journal` (balanced posting + balance invariant `Σ=0` + reversal + hash
> chain + `mb_account_balance`), `src/seed` (system-only + full starter chart per STARTER_CHART.md).
> Integration test proves the global identity (all postings sum to 0). `make leaks` clean (non-ASan
> build; ASan conflicts with the `leaks` tool). SQLite linked via system `-lsqlite3` (vendor the
> amalgamation before release).
>
> **BUILD STATUS (2026-06-15): Phase 2 accounting features COMPLETE — 41 tests green, 0 leaks.**
> Added: `src/counterparty` (clients/vendors CRUD), `src/item` (service/expense dictionaries, D14),
> `src/invoice` (DRAFT→issue posts Dr AR / Cr Income, tax line → Liability per D15, issued = locked),
> `src/bill` (symmetric: Dr Expense / Cr AP), `src/payment` (invoice receipt Dr Cash/Cr AR; bill
> payment Dr AP/Cr Cash; cumulative-payment status PARTIAL→PAID). Journal refactored: `post_core`
> (txn-less) + `mb_journal_post` (own txn) + `mb_journal_post_tx` (caller's txn) so invoice/bill/
> payment post + update atomically. Seeder records AR/AP/Tax/Opening account ids into `book_meta`.
> Added `mb_money_line_total` (rounded qty×price).
>
> **BUILD STATUS (2026-06-15): Phase 3 reporting COMPLETE — 47 tests green, 0 leaks.**
> `src/report`: core `mb_report_balances` (per-account net over a date window, via SUM(CASE) so the
> date filter actually excludes postings — a LEFT-JOIN condition would only null the entry), plus
> Trial Balance (debits==credits), P&L (income/expense/net), Balance Sheet (assets==liab+equity+net
> income identity), General Ledger (running balance seeded from opening), AR/AP aging (julianday
> buckets: current/1-30/31-60/61-90/90+), Cash Flow (in/out across ASSET+ACCOUNT).
>
> **BUILD STATUS (2026-06-17): Phase 4 gaps closed — Accounts editing + Items — 76 tests green.**
> - **Accounts & Categories editing:** engine `mb_account_update(id,code,name)` (type/role immutable);
>   API `account.update` + `account.set_active`. UI: `Accounts.jsx` gains a "+ New account/category"
>   form (friendly "what is it?" → type+role per D6), per-row **Edit** (rename/recode) + **Archive/
>   Restore**; SYSTEM accounts locked; row click still opens the ledger.
> - **Items dictionary (D14):** engine `mb_item_list`; API `item.create`/`item.list`/`item.set_active`.
>   UI: new **Items** tab (create service/expense template w/ default price + linked category, archive/
>   restore) and a **"+ Add from item…"** picker in the invoice/bill editor that autofills a line
>   (description, qty, price, category), filtered to Service for invoices / Expense for bills.
> - Tests: `account.update_renames_and_recodes`, `item.list_and_active_filter`. `make test` 76/76;
>   app/mcp relink clean; all three flows verified in preview. **Phase 4 is now complete.**
>
> **BUILD STATUS (2026-06-17): multi-company (D25) — 74 tests green.** One SQLite file = one
> company/book; no in-DB multi-tenancy (each book self-contained for backup + future per-book P2P).
> - **`src/registry`** — app-level book registry (JSON at `~/Library/Application Support/MoneyBooks/
>   books.json`; `MB_APP_DIR` override for tests): `mb_registry_default_path`/`_books_dir`/`_list`
>   (MRU-sorted)/`_touch`/`_forget`. Portable: only the config-dir lookup is platform-specific.
> - **`src/book`** — `mb_book_create(path,name,template)` (open→seed starter/empty→set company name;
>   refuses to clobber an existing file), `mb_book_company_name`/`_set_company_name` (book_meta).
> - **API** — `book.status` now returns `company_name`; new `book.set_name`.
> - **Shell (`src/app/main.c`)** — handles `app.*` at the shell level (swap active `mb_store` +
>   maintain registry): `app.book_current/book_list/book_create/book_open/book_forget`. Startup: path
>   arg → open it; no arg → open MRU book, else none → UI launcher. Non-`app.*` methods require an
>   open book (else MB_ERR_UNSUPPORTED "no book is open").
> - **UI** — `Launcher.jsx` (recent books / new company w/ template / open-by-path), App shows a
>   launcher when no book is current or via the sidebar **⇄ Switch company**; sidebar brand shows the
>   company name. Open/create → `window.location.reload()` re-inits against the new store.
> - Tests: `registry.add_update_list_forget`, `book.create_seeds_and_names`, `book.create_empty_template`.
>   `make test` 74/74; app/mcp relink clean; launcher + switch verified in preview.
> - **Known v1 gap:** no native NSOpenPanel (Phase 6); switching while an agent.send worker is in
>   flight could touch a closing store (rare — chat is disabled while busy; revisit if needed).
>
> **BUILD STATUS (2026-06-16): detail views + ledger + edit-until-paid — 71 tests green.**
> Added, per the user's request (spec updated: §4.3 edit model, §6.1 `transactions`/`category_transactions`
> views + drill-down, §7 detail views):
> - **Invoice/Bill detail view** (`ui/.../Invoices.jsx` — list rows are now click-through; one
>   `Detail` shows header + counterparty + line items + total + status badge). Engine: `mb_invoice_lines`
>   / `mb_bill_lines` (lines joined with account name); API `invoice.get` extended (counterparty_name +
>   memo + lines) and new `bill.get`.
> - **Edit until paid (D13 "edit on top").** DRAFT docs edit freely; an **OPEN (unpaid)** doc gets
>   "Reopen to edit" → `invoice.revert`/`bill.revert` posts a *reversing* entry (journal stays
>   immutable) and returns it to DRAFT, then re-issue/enter; PARTIAL/PAID/VOID are locked. Engine:
>   `mb_invoice_remove_line`/`_update`/`_revert_to_draft` (+ bill twins). A unified `DocEditor` drives
>   both create and edit; edit resyncs lines (engine has no line-update: drop existing + re-add).
> - **Transactions screen** (`Transactions.jsx`, new tab) — All = `report.journal` (every entry, audit
>   view incl. reversals); Income/Expense = `report.category_txns` (posting-level, POSTED only, with
>   total). Engine: `mb_report_journal`, `mb_report_category_txns`; API `report.journal`,
>   `report.category_txns`, and `report.ledger` (per-account, exposes the pre-existing `mb_report_ledger`).
> - **Dashboard drill-down** — Income/Expenses cards are click-through → Transactions with that filter
>   (App now carries a `{tab, params}` nav + `go()`).
> - **Transactions → All: Type + accounting-style amounts** — `mb_report_journal` now classifies each
>   entry by the account types it touches (`flow` = INCOME / EXPENSE / OTHER, via a SQL CASE+EXISTS);
>   the All view shows a colored Type column (Income green / Expense red / Transfer muted) and renders
>   money-out (EXPENSE) in accounting parentheses + red, income green, transfers neutral. A payment
>   receipt (cash↑/AR↓) is OTHER → "Transfer".
> - **Per-account General Ledger** (`Accounts.jsx`) — clicking an account opens its ledger via
>   `report.ledger`: Debit/Credit columns + running balance, presented in the account's **natural sign**
>   (credit-normal accounts negate the debit-positive engine running balance). Completes the
>   `general_ledger` view from SPEC §6.1.
> Tests added: `invoice.list/edit_draft_lines_and_reopen_open`, `report.journal_lists_all_entries`,
> `report.category_txns_income_and_expense`, `api.onboarding_seeds_once` (earlier). `make test` 71/71;
> `make app` + `make mcp` relink clean. UI flows verified in the Vite preview (dashboard→txns deep-link,
> invoice detail, reopen→edit pre-fill).
>
> **BUILD STATUS (2026-06-16): Phase 4 UI COMPLETE — 68 tests green.** Finished the three
> remaining UI items. **(1) Invoices & Bills tab** (`ui/src/components/Invoices.jsx`): one component
> drives both sides via a per-kind config; lists existing docs with a status badge + total, a create
> form builds a DRAFT with dynamic line items then issues/enters it in one flow, and OPEN/PARTIAL
> docs get an inline "Record payment". Needed new engine list functions `mb_invoice_list` /
> `mb_bill_list` (header rows + counterparty name + computed total, JOIN+subquery, newest-first) and
> API methods `invoice.list` / `bill.list`. **(2) First-run wizard** (`Wizard.jsx`, D7): `main.c` no
> longer silently auto-seeds; the UI calls `book.status` and, if the book has no chart, shows
> "starter template vs start empty" → `book.onboard {template}`. `book.onboard` only seeds an empty
> book (seeding isn't idempotent) and sets an `onboarded` flag; `book.status` reports onboarded when
> the flag is set **OR** the chart is non-empty (so pre-existing seeded books skip the wizard and
> never double-seed). **(3) Dashboard customization** (`Dashboard.jsx`): show/hide the non-hero
> widgets via a Customize toggle, persisted to `localStorage`; Cash position stays the fixed hero.
> Tests: `invoice.list_carries_name_status_total`, `bill.list_carries_vendor_status_total`,
> `api.onboarding_seeds_once`. `make test` 68/68; `make app` + `make mcp` relink clean.
>
> **BUILD STATUS (2026-06-15): Phase 4 UI shell — verified parts COMPLETE (51 tests green).**
> - **JSON API bridge** `src/api` (cJSON vendored under `src/vendor/cjson`, warning-isolated via
>   `src/json/json_vendor.c`): `mb_api_dispatch(method, args_json) -> result_json`; methods
>   account.create/get/list + report.trial_balance/pnl/balance_sheet/cash_flow; always returns JSON
>   ({"error":{code,message}} on failure). **Shared contract for BOTH the WebView UI and the MCP layer.**
> - **React/Vite UI** in `ui/` (D24): Dashboard (cash-position hero), Accounts, Reports; `api.js`
>   bridge calls `window.mbInvoke` with a browser mock fallback. `npm run build` → 148KB JS (47KB gz).
> - **Native shell** `src/app/main.c`: WKWebView via `webview/webview` (vendored by
>   `scripts/fetch_webview.sh`); binds `mbInvoke`→`mb_api_dispatch`; first-run seeds starter chart.
>   `make app` **verified it links (240KB binary)**; needs `-DWEBVIEW_STATIC` + `-lc++ -framework
>   WebKit -framework Cocoa`. GUI window displays only on a logged-in macOS session (headless shell
>   can't render it). `make ui` builds the front-end.
> **WebView bridge gotchas (learned the hard way, all fixed):**
>   1. **`-DWEBVIEW_STATIC`** required on both the C++ webview compile and the C link, else the C API
>      defaults to `inline` and the symbols vanish (undefined `_webview_create` at link).
>   2. Include **`<webview/api.h>`** (C-safe) from C — NOT `<webview/webview.h>` (pulls in `.hh` C++).
>      Link with the **C driver + `-lc++`** (webview impl is C++).
>   3. **Load the UI as a single inlined HTML.** WKWebView over `file://` (origin "null") blocks
>      `type="module"`/`crossorigin` sibling assets → blank white window. Fixed with
>      **vite-plugin-singlefile** (inlines JS+CSS into one index.html).
>   4. **webview auto-JSON-parses the `webview_return` value**, so a bound JS fn resolves to a parsed
>      object — the JS bridge must NOT `JSON.parse` it again (caused `JSON Parse error: ... "object"`).
>   5. `webview_create(debug=1)` enables the Web Inspector (flip to 0 for release).
> The binary loads `ui/dist/index.html` at runtime → front-end changes need only `npm run build`
> (rebuild `ui/`), no relink.
> Phase 4 write methods + a **Record** income/expense UI form also wired (income/expense.record,
> transaction.post, counterparty.*, invoice.*, bill.*, payment.record through `mb_api_dispatch`).
>
> **BUILD STATUS (2026-06-15): Phase 5 MCP server (stdio) COMPLETE — 59 tests green, 0 leaks.**
> `src/mcp/mcp.c`: transport-agnostic `mb_mcp_handle` (JSON-RPC 2.0, protocol 2025-11-25) —
> initialize / ping / tools/list / tools/call / notifications. **20 tools**, each mapping 1:1 to an
> `mb_api_dispatch` method (the surface shared with the UI). Per-tool **Permit/Ask/Block** (D18) via
> migration **v3** `tool_permission` table: defaults read=PERMIT/write=ASK; BLOCK hides from
> tools/list + refuses tools/call (server-side); over stdio, Ask is the client's job. stdio binary
> `src/mcpd/main.c` → `make mcp` (pure C, no webview); smoke-tested over a pipe (initialize +
> tools/list = 20). Claude Desktop config + tool list in [MCP.md](MCP.md).
>
> **BUILD STATUS (2026-06-15): in-app agent CORE (privacy + orchestration) COMPLETE — 63 tests
> green, 0 leaks.**
> - **`src/redact`** (D10/D11 egress choke point): `mb_redactor` built from the book; `mb_redact`
>   swaps counterparty names → Client_N/Vendor_N and money-account (role=ACCOUNT) names → Account_N;
>   `mb_restore` reverses. Category/system names + amounts pass through. Longest-key-first so
>   Client_1/Client_10 prefix overlap round-trips safely. Tested.
> - **`src/agent`** (D9 orchestration): `mb_agent_run(store, provider, msg)` — provider-agnostic
>   tool-use loop with an **injectable** `mb_llm_provider` fn-ptr (so it's testable with a mock).
>   Redacts the user msg AND tool results before they reach the provider; executes tool calls via
>   `mb_api_dispatch`; restores tokens in the final reply. Protocol: req `{messages,tools}` → resp
>   `{tool_calls[]}` or `{text}`. Mock tests prove tool execution posts to the books + redaction.
> - **`src/llm`** (real provider, build-verified): `mb_llm_openai_provider` / `..._from_env` —
>   OpenAI-compatible dialect over **libcurl** (covers OpenAI + OpenRouter), translates the agent
>   protocol ↔ Chat Completions (messages/tool_calls/tools), non-streaming. `agent.send` API method
>   runs `mb_agent_run` with the env-configured provider. **Sidebar Assistant** chat UI (ui/). App +
>   mcp relink with `-lcurl`. Setup/run in [AGENT.md](AGENT.md).
> 63 tests green (provider/UI are build-only — need a key/network; run on the user's Mac via
> `MB_LLM_API_KEY=... ./build/MoneyBooks book.sqlite`).
> **Keys now stored properly (D22 BUILT):** `src/secret` Keychain backend + Settings UI (see D22).
> **UI polish (2026-06-15):** `agent.send` now runs on a **worker thread** (pthread →
> `webview_dispatch` back to main) so the window no longer freezes; chat shows an animated typing
> indicator; Settings provider fields fixed to stack vertically (CSS was scoped to `form`, Settings
> cards aren't a form → broadened to `.card`).
> **Remaining (Phase 5 cont.):** reply **streaming** (token-by-token); Anthropic-native dialect
> (OpenRouter covers Claude meanwhile); "Ask"-confirmation UI for in-app tools; embedded HTTP/SSE
> MCP transport; per-tool-policy settings screen.


- **D1 — UI engine:** System **WKWebView** (macOS WebKit), driven from the C core via a thin
  Objective-C shim. "Tauri for C" model. No bundled Chromium; no custom renderer.
  _(Confirmed 2026-06-12.)_
- **D2 — Primary user / scope:** **Personal / freelance.** Single set of books (single-company),
  tracking service income + expenses. Multi-company explicitly out of scope for v1.
  _(Confirmed 2026-06-12.)_
- **D3 — Currency:** **Single currency** for v1 (no FX). Design note: store a currency code on
  the book and keep money fields currency-ready so multi-currency can be added later without a
  painful migration, but build/test single-currency only. _(Confirmed 2026-06-12.)_
- **D4 — Accounting basis:** **Accrual.** Record income when invoiced (→ Accounts Receivable)
  and expenses when billed (→ Accounts Payable); match payments against invoices/bills. Requires
  invoice & bill entities + AR/AP accounts. _(Confirmed 2026-06-12.)_
- **D5 — Invoice scope (v1):** **Record-only** (accounting records with client, line items from
  services dictionary, dates, status). No PDF/email in v1. **Forward constraint:** data model &
  invoice entity must be designed so PDF generation and emailing can be layered on later without
  reshaping the schema. _(Confirmed 2026-06-12.)_
- **D6 — Account model / UX:** One **pure double-entry chart of accounts** at the core (5 types:
  Asset/Liability/Equity/Income/Expense). UI defaults to a **friendly view** ("Accounts" =
  asset/liability/where-money-is; "Categories" = income/expense/what-it's-for) with an **advanced
  toggle** exposing the full chart + types. Friendly labels are a presentation mapping, not a
  separate data model. _(Confirmed 2026-06-12.)_
- **D7 — New-book seeding:** **First-run wizard** offers "starter template (editable)" or "start
  empty." Freelancer starter chart **DESIGNED & approved** → [STARTER_CHART.md](STARTER_CHART.md)
  (US software/consulting, Schedule C-aligned, JSON seed; "start empty" still creates the 4 SYSTEM
  accounts). _(Confirmed 2026-06-12; chart approved 2026-06-14.)_
- **D8 — AI/MCP runtime topology (single-writer):** The **accounting engine (C library) is the
  ONLY writer**; all writes go through it so double-entry invariants always hold — no process ever
  writes the SQLite file with raw SQL. Layers:
  1. **Engine** — C library over SQLite, enforces invariants. Must support a **headless mode**
     (run without the WKWebView window).
  2. **Embedded MCP server** — runs in-app on localhost (HTTP/SSE); canonical AI interface; the
     sidebar agent and external LLMs use it; writes go through the engine → live UI updates.
  3. **stdio proxy shim** — small binary for standard MCP clients (e.g. Claude Desktop); forwards
     MCP calls to the engine. Never writes the DB directly. **When the app is closed it
     auto-launches the engine headless**, preserving the single-writer guarantee.
  _(Confirmed 2026-06-12. Implication: clean separation of engine vs UI shell; engine runnable
  headless.)_
- **D9 — LLM provider layer:** ⚠️ **SUPERSEDED 2026-06-19 — the in-app sidebar agent was removed.**
  AI access is now solely through the MCP server (external client, e.g. Claude Desktop); the engine
  makes no LLM calls of its own. _(Original: built-in sidebar agent with a pluggable provider
  abstraction — Anthropic/OpenAI/OpenRouter, BYO key. Confirmed 2026-06-12.)_
- **D10 — Privacy posture (AI egress):** ⚠️ **SUPERSEDED 2026-06-19 — with the in-app agent removed,
  the engine has no cloud egress; the redaction/pseudonymization machinery below was deleted.** Data
  now leaves the device only via the user's own external MCP client, under that client's control.
  _(Original posture — minimize + redact, cloud OK — preserved below for history.)_ Storage stays
  100% local; egress happens ONLY on sidebar-agent LLM calls. Required machinery:
  1. **Redaction/pseudonymization boundary** — swap sensitive identifiers (e.g. client/counterparty
     names, account numbers, memo/notes text) for stable tokens (`Client_7`) before sending; map
     back in the response. Model reasons over structure, not identities.
  2. **Minimization** — agent tools return aggregates/summaries and least-needed rows, not full
     ledgers, by default.
  3. **Single auditable egress choke point** — one code path all provider traffic passes through;
     optional "preview what will be sent" + an egress log.
  4. Provider notes: Anthropic/OpenAI **API** don't train on data by default; **OpenRouter is a
     proxy** (weakest link) → flag/limit it for sensitive content.
  **Spec task:** define the exact default "sensitive fields" set + redaction rules.
  _(Confirmed 2026-06-12.)_
- **D11 — Default sensitive fields:** Pseudonymize by default: **client/counterparty names** and
  **account names/numbers (IBAN, card last-4)**. **Pass through** (not redacted by default): memos/
  descriptions/notes and exact amounts — chosen to keep the agent useful. Residual risk: free-text
  memos can contain names → provide an easy **opt-in toggle to redact memos** later. Amounts always
  pass through so the agent can compute. _(Confirmed 2026-06-12.)_
- **D12 — Money representation:** **Integer minor units, 2 decimals everywhere** (store amounts as
  integer cents). No floating point for money — ever. Unit prices/quantities also 2dp; line totals
  computed and rounded to the cent. Keep a currency code on the book (D3) so storage is
  currency-ready. _(Confirmed 2026-06-13.)_
- **D13 — Correction model:** **Immutable / append-only journal.** Entries are never edited or
  deleted in place; corrections are **reversing + new entries**. UI presents friendly Edit/Delete
  that post reversals under the hood; advanced view shows real reversal history. Implications:
  append-only entry/posting tables, entry status (posted/reversed), audit fields; **MCP write tools
  must also correct via reversal, never mutate**. Foundation for audit trail + P2P/CRDT merge.
  _(Confirmed 2026-06-13.)_
- **D14 — Dictionaries = item/template layer:** Separate **Item** entities sit above categories.
  A **service item** = {name, default unit price, linked income category, unit label, active}.
  An **expense item** = {name, default amount, linked expense category, active}. Items are
  reusable: dropped onto invoices as line items / picked for fast expense entry, and exposed to the
  AI ("add my usual consulting line"). Line items reference an item but can be free-form too.
  _(Confirmed 2026-06-13.)_
- **D15 — Tax (v1):** **No tax engine.** US consulting/software services are generally NOT
  sales-taxable (state-dependent; confirm w/ CPA). When tax IS needed, add it as an **optional
  line item posting to a "Sales Tax Payable" LIABILITY account** (not income) — that account's
  balance = tax owed/to remit. Income/self-employment tax is NOT an invoice line; it's derived from
  profit at filing time (future reporting helper). Schema stays ready for a future multi-rate
  engine. _(Confirmed 2026-06-13.)_

- **D16 — v1 reports:** Balance Sheet, P&L, **AR aging (+ AP aging)**, **Trial Balance**,
  **General Ledger / per-account register (drill-down w/ running balance)**, and a **Cash Flow
  summary** — all with date-range & category/account filtering. Note: accrual cash flow is the
  fiddly one → spec a pragmatic cash-in/cash-out version first, refine later. GL drill-down doubles
  as the view the AI can cite. _(Confirmed 2026-06-13.)_

- **D17 — Flexible dashboard (widgets over views):** **Reports and dashboard widgets share one
  abstraction** — every report is a named, parameterized **view** producing structured data + a
  display type (big number / list / chart / table). The **Reports section lists all views**
  (nothing is ever lost). The **dashboard is an ordered set of widget instances**, each referencing
  a view with a **size (hero/normal)** + config; layout saved locally. **Cash position = default
  hero widget.** v1 customization: **add/remove + reorder + size** (full drag-grid is a fast-follow).
  AI-native bonus: **the agent can define a custom view and pin it** as a widget. _(Confirmed 2026-06-13.)_

### Data-model items to specify (no decision needed — standard, will go straight into spec)
- **Transaction = header + balanced postings (splits):** each transaction has ≥2 postings; each
  posting hits one account with a signed amount; sum of postings = 0 (balanced). Multi-split
  capable (a payment can hit several categories). This is the canonical double-entry shape.
- **IDs & audit:** stable UUIDs on every entity; created/posted timestamps; reversal links; entry
  status. (Supports immutability D13 + future P2P D-future.)
- **Reports as views:** report engine produces structured, parameterized view results reused by
  both the Reports section and dashboard widgets (per D16/D17); same results are what the MCP report
  tools return and what the agent can pin.

- **D18 — Agent authority = per-tool permission policy (Permit / Ask / Block):** Like Claude
  Desktop connector permissions. Each tool has a setting:
  - **Permit** — runs automatically, no prompt.
  - **Ask** — prompts the user inline to approve/reject each call (shows the proposed change).
  - **Block** — tool is disabled for the agent.
  Defaults: **all read/report tools = Permit**; write/mutating tools = **Ask** (user-configurable),
  nothing Block by default. Enforcement is **server-side** in the engine/MCP layer (so it holds for
  the sidebar agent AND external MCP clients), with the sidebar surfacing "Ask" prompts. A
  **settings screen lists every tool with the 3-state control.** **Factory default for write tools
  = Ask** (reads = Permit, nothing Block). _(Confirmed 2026-06-13.)_

- **MCP tool surface (near-given, no decision):** expose **high-level intent-based tools** mirroring
  vetted engine operations (`record_income`, `record_expense`, `create_invoice`, `record_payment`,
  `add_account`, `add_item`, `run_report`, `list_*`, etc.) — **never raw table CRUD / SQL**. All AI
  writes go through invariant-enforcing engine code (ties to D8/D13). Pseudonymization map (D11)
  stored in a **local table**, applied at the egress choke point.

- **D19 — Validated tech stack (from research, see [RESEARCH.md](RESEARCH.md)):** All unknowns
  confirmed feasible in pure C. Stack: **UI** `webview/webview` (MIT) → system WKWebView, `objc_msgSend`
  escape hatch; **store** SQLite amalgamation (public domain), WAL + `BEGIN IMMEDIATE` +
  `busy_timeout`, FTS5 flag, JSON1 default, `user_version` migrations, online-backup API; **JSON**
  yyjson or cJSON (MIT); **HTTP client** libcurl; **MCP** built from scratch (JSON-RPC 2.0, target
  rev 2025-11-25), embedded HTTP/SSE via **civetweb (MIT — NOT mongoose/GPL)**, stdio shim forwards
  to live app; **LLM** two dialects only (OpenAI≈OpenRouter, Anthropic) over libcurl+SSE; **P2P
  (future)** `iroh-c-ffi` for QUIC, append-only-log + version-vector sync (no CRDT). Rust toolchain
  needed ONLY at the future iroh phase. _(Confirmed 2026-06-13.)_
- **D20 — Sync-ready entry identity (design-now constraint):** Even though P2P ships later, **every
  journal entry carries from v1**: stable `device_id`, per-device monotonic `seq`, a Lamport clock,
  and content + prev-entry hashes (tamper-evident hash chain). Makes the append-only log directly
  syncable (D13 + research §5) and is expensive to retrofit, ~free to bake in now. _(Confirmed
  2026-06-13.)_

- **D21 — Phase 0 testing/error/build (BUILT, see [PHASE0.md](PHASE0.md)):** Testing & memory
  hygiene start at commit #1, designed **AI-first** (failures located, self-explaining,
  machine-readable). Implemented & green (`make test` under `-Werror` + ASan + UBSan; `money`
  module proves it):
  - **Harness:** custom, ~auto-registering (`TEST()` + `__attribute__((constructor))`), Rust-style
    (unit tests in-file under `#ifdef MB_TEST`, integration tests in `tests/`). Runner: `--filter`,
    `--json`, `--list`, crash-names-test, per-failure re-run hint. Value-printing assertions
    (`ASSERT_MONEY_EQ` → `15.00 != 16.00`).
  - **Errors:** two-channel — `mb_err` codes + thread-local located last-error w/ `MB_TRY`
    breadcrumb trail; `MB_INVARIANT` (always-on integrity) vs `MB_DEBUG_ASSERT`. **`MB_MUST_CHECK`
    (`warn_unused_result`) makes ignoring an error a compile error** (verified).
  - **Build:** clang/C11; `make test|release|leaks|analyze|deadcode|clean`. macOS uses Apple
    `leaks` (Valgrind/LSan weak on arm64).
  - **Dead-code: 3 nets** — compile warnings + `MB_MUST_CHECK`; `make deadcode` (coverage → 0-hit
    functions); `make analyze` (Clang Static Analyzer).
  - Known gotcha: deleting a source needs `make clean` (no dep tracking yet).
  _(Built 2026-06-13. First real module: `src/money` — integer cents, D12.)_

- **D22 — Secret storage (portable abstraction):** ⚠️ **SUPERSEDED 2026-06-19 — `src/secret` was
  deleted with the in-app agent (it only held LLM API keys, so it became dead code). No secret store
  ships in v1; reintroduce it behind this seam if a future feature needs OS credential storage.**
  _(Original below for history.)_ Provider API keys live behind an
  **`mb_secret_store`** interface, **never in SQLite/logs/egress** (the `.sqlite` is the
  export/sync artifact, so keys there would leak). Backends: **macOS Keychain (built now)**;
  Windows Credential Manager/DPAPI + Linux libsecret (later); **encrypted-file fallback** for
  headless/no-keystore; **env-var override** everywhere. Note: only the GUI sidebar agent needs
  keys — the headless/stdio-shim path doesn't (external client brings its own LLM). _(Confirmed
  2026-06-13. **BUILT 2026-06-15:** `src/secret` — macOS Keychain backend (Security.framework) +
  in-memory backend for tests (`-DMB_SECRET_MEMORY`); `app_setting` table (migration v4) for non-
  secret provider config; `settings.list_providers`/`set_provider`/`clear_key` API; **Settings UI**
  to add keys per provider (OpenAI/OpenRouter); `agent.send` reads the active provider's key from
  Keychain, env only as fallback. 65 tests green, 0 leaks.)_
- **D23 — Portability principle (forward-looking):** **Ship Mac/M-chip first, but design portably.**
  Engine = portable C; platform-specific code (secret-store backend, UI chrome/`objc_msgSend`,
  paths) behind **thin abstractions**. Most of the stack already ports (webview/webview→WebKitGTK/
  WebView2, SQLite, libcurl, civetweb, yyjson, iroh, msquic). **Linux & Windows are explicit future
  targets** (not v1). Cheap now, costly to retrofit. _(Confirmed 2026-06-13.)_
- **D24 — Front-end stack: React + Vite.** UI built as a **React** app (Vite build) compiled to
  static assets bundled in the app and loaded in the WebView; talks to the C engine over the C↔JS
  bridge. Biggest ecosystem (charts/forms/components for reports+dashboard). Trade-off: adds a Node/
  Vite build step to the toolchain and a larger (still WebView-scale, not Chromium) JS bundle.
  Portable (just static assets). Preact is a drop-in lighter fallback if bundle size bites. _(Confirmed 2026-06-14.)_
- **D27 — Cross-platform: one codebase, compile-time selection (NOT separate forks).** Realizes D23.
  Keep a **single `src/` tree**; pick the per-OS implementation at **compile time**
  (`#if defined(__APPLE__) / _WIN32 / __linux__`) behind the thin seams that already exist —
  **paths** (`src/registry`), **native shell/dialogs** (`src/app`; `webview/webview` already abstracts
  the WebView → Cocoa/WebKitGTK/WebView2), and **build**. We do **not** maintain a forked "Windows
  build" of the Mac app: the engine (double-entry, SQLite, MCP, reports) is ~95% identical everywhere,
  so forking would duplicate the shared bulk and drift. What's genuinely per-OS is **build + packaging
  + signing**, not source: one repo → signed `.app` (mac) / MSI (Windows) / AppImage·deb (Linux). The
  Mac `Makefile` (clang + `-framework`) is the one Mac-specific artifact and moves to **CMake** in
  Phase 6. **Sequencing: Phase 7 (P2P/iroh) first, then Phase 6 (packaging + cross-platform).**
  _(Decided 2026-06-19.)_

---

## 5. Open Questions Queue (ordered; ask one at a time)

**Answered → see Decision Log:** Q1→D1, Q2→D2, Q3→D3, Q4→D4, Q5→D5, Q6→D6, Q7→D7,
Q8→D8, Q9→D9, Q10(privacy posture)→D10, Q11(sensitive fields)→D11.

**Remaining clusters (not yet asked):**
- **Reporting:** v1 depth — BS + P&L + filters only, or also Trial Balance / General Ledger /
  per-account ledgers / cash flow? Date-range & dimension filtering.
- **Data model details:** money representation (lean: integer minor units), entry/posting schema,
  append-only journal + reversing entries, IDs, audit fields, immutable vs soft-delete.
- **Dictionaries:** services-provided + expense dictionaries; relation to invoice line items & categories.
- **Non-AI UX:** dashboard contents, "few buttons" primary flows (record income/expense, new
  invoice, mark paid, reconcile?), navigation.
- **MCP tool surface:** which read/write/report operations to expose + the pseudonymization map store.
- **Storage/ops:** DB file location, backups, export/import, schema migrations.
- **Attachments:** receipts/invoice files — paths vs blobs, or out of scope v1?
- **P2P / sync (future):** iroh + VPS role; append-only/CRDT implications (design-forward, not v1 build).
- ~~**Build & test tooling**~~ → **DONE in Phase 0 (D21).** Makefile (clang/C11), custom test
  harness, error model, sanitizers, dead-code tooling all built. (CI wiring still TODO.)
- **Packaging:** .app bundle, signing/notarization (later), single binary vs engine+shim.

_(Queue will grow/reorder as we talk.)_

---

## 6. Answers Log (chronological)

- **2026-06-12 — Q1 (UI engine):** System WebView → became **D1**.
- **2026-06-12 — Q2 (primary user):** **Me — personal/freelance.** One set of books,
  tracking service income + expenses. → **D2.**
- **2026-06-12 — Q3 (currency):** **Single currency** for v1. → **D3.**
- **2026-06-12 — Q4 (basis):** **Accrual** — record on invoice/bill, match payments.
  Implies AR/AP + invoice/bill entities. → **D4.**
- **2026-06-12 — Q5 (invoice scope):** **Record-only for v1**, but architect so document
  generation (PDF) + email can be added later. → **D5.**
- **2026-06-12 — Q6 (account model):** **Both/toggle** — friendly "Accounts/Categories" view by
  default, advanced mode reveals full chart of accounts + 5 types. One double-entry core. → **D6.**
- **2026-06-12 — Q7 (seeding):** **Ask on first run** — wizard offers starter template (editable)
  or empty. Need to design a freelancer starter chart. → **D7.**
- **2026-06-12 — Q8 (MCP topology):** User wanted "both" but worried about direct DB access.
  Resolved to **single-writer** design: engine is the only writer; embedded HTTP MCP is canonical;
  stdio shim is a proxy (never raw DB writes). **Q8b:** when app closed, shim **auto-launches a
  headless engine**. → **D8.**
- **2026-06-12 — Q9 (agent brain):** **Pluggable providers — Anthropic + OpenAI + OpenRouter**
  (BYO key). User added a hard requirement that sensitive data must not leak → opened a **Privacy
  sub-thread** (deep discussion). → **D9 + Privacy sub-thread.**
