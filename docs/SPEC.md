# Money Books — Specification (v1 draft)

> Status: **living spec**, drafted from the decision log (D1–D21), technical research, and the
> built Phase 0 foundations. Companion docs:
> [PROJECT_NOTES.md](PROJECT_NOTES.md) (decisions), [RESEARCH.md](RESEARCH.md) (verified tech),
> [PHASE0.md](PHASE0.md) (testing/error/build). Date: 2026-06-13.
>
> Sections marked **(TODO)** await decisions on the remaining lighter clusters.

---

## 1. Vision

A simple, **AI-native, local-first double-entry accounting app** for macOS (Apple Silicon),
written in **pure C** with a **web-like native UI**. It pairs a rigorous double-entry engine with
an integrated **MCP server**, so the books can be read and manipulated by an external LLM client
through the same vetted operations a human uses — safely, auditably, and offline.

**Product principles**
1. **Correct before clever** — the books are always balanced and audit-true; integrity beats features.
2. **Few buttons** — a clean dashboard and a handful of primary flows, not a toolbar jungle.
3. **AI-native, not AI-bolted-on** — the operation set *is* the tool surface; MCP exposes it.
4. **Local-first** — all data lives on device; the network is optional and only for AI + (future) sync.
5. **Your data, your control** — explicit, per-tool AI permissions and a single auditable egress point.
6. **Portable by design (D23)** — ship Mac/M-chip first, but keep the engine in portable C and all
   platform-specific code behind thin abstractions; **Linux & Windows are explicit future targets.**

---

## 2. Scope

### In scope (v1)
- **Single user / freelance** (D2), **single currency** (D3, schema currency-ready).
- **Multiple companies** (D25): one SQLite file = one company/book; an app-level **registry** of
  known books + a **launcher** to create / open / switch. No `company_id` multi-tenancy in one DB —
  each book stays self-contained (clean backup + future per-book P2P sync).
- **Accrual** double-entry with **invoices, bills, and AR/AP** (D4).
- Invoices/bills **recorded** (not generated/sent) — PDF/email-ready data model (D5).
- User-definable **chart of accounts** with a friendly view + advanced toggle (D6); first-run wizard (D7).
- Reusable **service/expense item dictionaries** (D14); optional **tax-as-liability line** (D15).
- Reports: **Balance Sheet, P&L, Trial Balance, General Ledger, AR/AP aging, Cash Flow** (D16),
  with filtering, surfaced as a **flexible widget dashboard** (D17).
- **MCP server** exposing the engine's intent operations to an external LLM client (e.g. Claude
  Desktop), with **per-tool Permit/Ask/Block** permissions (D18). *(The in-app sidebar agent of D9
  was built and then removed — see §8.4.)*
- **Phase 0 testing/error/build** foundation (D21), already built.

### Out of scope (v1, designed not to preclude)
- Multi-**user** / collaboration on one book; multi-currency & FX; invoice PDF/email; full tax
  engine; P2P sync (iroh) and VPS assist; mobile/Windows/Linux; native file-picker for opening
  arbitrary book files (v1 creates books in the app dir + opens known/by-path; NSOpenPanel is Phase 6).

---

## 3. Architecture

### 3.1 Layered, single-writer

```
            ┌──────────────────────────────────────────────────────────┐
            │                     Money Books .app                       │
            │                                                            │
   UI  ◄────┤  WKWebView (HTML/CSS/JS)  ⇄  C↔JS bridge (JSON-RPC-ish)   │
            │                              │                             │
            │                    ┌─────────▼──────────┐                  │
            │                    │   Accounting       │ ◄─ Embedded MCP  │◄── external LLM
            │                    │   ENGINE (C lib)   │    server (HTTP/  │    (Claude Desktop
            │                    │   = the ONLY       │    SSE, 127.0.0.1)│     via stdio shim)
            │                    │   writer           │                  │
            │                    └─────────┬──────────┘                  │
            │                              │ SQLite (WAL, amalgamation)   │
            └──────────────────────────────────────────────────────────┘
                          ▲ stdio proxy shim (separate tiny binary): forwards MCP
                            calls to the live app; auto-launches engine headless if closed
```

**The engine is the only writer** (D8). Every mutation — from the UI or an external MCP client —
goes through the engine's invariant-enforcing operations. No component ever issues raw SQL writes.
This is what makes the AI safe to empower.

- **Engine (C library):** owns SQLite, enforces double-entry invariants, exposes the operation set.
  Must run **headless** (no window) so the stdio shim can auto-launch it.
- **UI shell:** WKWebView via `webview/webview`; calls engine ops over the C↔JS bridge.
- **Embedded MCP server:** in-process, localhost HTTP/SSE; the AI interface; calls engine ops.
- **stdio proxy shim:** tiny separate binary for standard MCP clients; forwards to the live app via
  local IPC; **auto-launches the engine headless** when the app is closed (D8). Never writes the DB.

### 3.2 Tech stack (verified — RESEARCH.md)

| Concern | Choice | License |
|---|---|---|
| Native UI | `webview/webview` → system WKWebView | MIT |
| UI escape hatch | raw `objc_msgSend` (native menus/dialogs) | system |
| Store | SQLite amalgamation, WAL, `BEGIN IMMEDIATE`, FTS5, JSON1, `user_version` | Public domain |
| JSON | yyjson (or cJSON) | MIT |
| MCP HTTP/SSE server (future) | civetweb (**not** mongoose/GPL) | MIT |
| MCP protocol | hand-built JSON-RPC 2.0, target rev **2025-11-25** | — |
| P2P (future) | `iroh-c-ffi` + append-only-log/version-vector sync | Apache-2.0/MIT |

Pure C throughout; a Rust toolchain enters **only** at the future iroh phase.

### 3.3 Portability (D23)

Ship Mac/M-chip first, **design portably**. The engine is portable C; platform-specific code sits
behind thin abstractions:
- **UI chrome glue** — native menus/dialogs via `objc_msgSend` (mac); per-OS equivalents later.
- **Paths** — data/config/cache dir resolution per OS (`src/registry` is already the single seam).

*(A `mb_secret_store` Keychain abstraction existed for the in-app agent's API keys; it was removed
with the agent. If a future feature needs OS secret storage, reintroduce it behind this seam.)*

Nearly the whole stack already ports — `webview/webview` (WebKitGTK/WebView2), SQLite, civetweb,
yyjson, iroh, msquic. **Linux & Windows are explicit future targets, not v1.**

**Cross-platform strategy (D27): one codebase, compile-time platform selection — NOT separate
forks.** The portable engine (double-entry, SQLite, MCP, reports, JSON) is ~95% of the code and is
identical on every OS; only a few seams differ. So we keep a **single source tree** and select the
right implementation at **compile time** (`#if defined(__APPLE__) / _WIN32 / __linux__`) behind the
existing thin abstractions — never a forked "Windows version." Forking would duplicate the shared
95% and guarantee drift. Concretely, the per-OS seams are:
- **Paths** — app-data/config dir (`src/registry`): macOS `~/Library/Application Support`, Linux
  XDG (`~/.local/share`), Windows `%APPDATA%`.
- **Native shell glue** (`src/app`): window + file-open dialog — `objc_msgSend`/Cocoa (mac),
  GTK (Linux), Win32 (Windows). `webview/webview` already abstracts the WebView itself.
- **Build** — the macOS `Makefile` (clang + `-framework`) is the one genuinely Mac-specific artifact;
  cross-platform builds move to **CMake** (or per-OS make fragments) so each OS links its own
  frameworks/libs from the same sources. This is a Phase-6 (packaging) task, after Phase 7.

What is **per-platform** is the *build + packaging + signing*, not the *code*: one repo produces a
signed `.app` (mac), an installer/MSI (Windows), and an AppImage/deb (Linux) from the same `src/`.

---

## 4. Data model

> All amounts are **integer cents** (`mb_money = int64_t`, D12), built & tested in Phase 0.
> The journal is **append-only/immutable** (D13): corrections are reversing + new entries.
> Every syncable row carries **sync identity** (D20): `device_id`, `seq`, `lamport`, `content_hash`,
> `prev_hash`. Stable IDs are UUID text.

### 4.1 Core double-entry

**`account`** — the chart of accounts (one model; friendly labels are presentation, D6).
| field | notes |
|---|---|
| `id` | UUID |
| `name` | user-editable |
| `type` | `ASSET` / `LIABILITY` / `EQUITY` / `INCOME` / `EXPENSE` |
| `friendly_role` | `ACCOUNT` (where money is: asset/liability) / `CATEGORY` (what for: income/expense) / `SYSTEM` |
| `parent_id` | optional hierarchy |
| `code` | optional account number |
| `is_active` | archive instead of delete |
| `currency` | book currency (D3) |
| sync identity (D20) | |

**`journal_entry`** — a transaction header (immutable).
| field | notes |
|---|---|
| `id` | UUID |
| `date` | transaction date |
| `memo` | free text (note: passes through to AI by default, D11) |
| `status` | `POSTED` / `REVERSED` / `REVERSAL` |
| `reverses_id` | if this entry reverses another |
| `source` | `USER` / `AI` / `IMPORT` |
| `created_at` | wall clock |
| sync identity (D20) | hash chain ties entries tamper-evidently |

**`posting`** — the balanced splits of an entry (≥2 per entry).
| field | notes |
|---|---|
| `id` | UUID |
| `entry_id` | FK → journal_entry |
| `account_id` | FK → account |
| `amount` | signed cents: **+ = debit, − = credit** |
| `memo` | optional per-line note |

**Invariant (enforced + `MB_INVARIANT`):** for every entry, `Σ posting.amount = 0`. A write that
would unbalance returns `MB_ERR_UNBALANCED` and commits nothing (wrapped in `BEGIN IMMEDIATE`).

**Storage vs display (confirmed):** postings store a **single signed amount** (`+`=debit, `−`=credit;
`SUM=0`) for simplicity and a bulletproof invariant. The accountant-facing **debit/credit columns**
(Trial Balance, journal view, advanced mode) are *derived* at display time (`amount ≥ 0` → debit
column, `< 0` → credit column). Signed under the hood, Dr/Cr on screen — both, no compromise.

### 4.2 Counterparties & items

**`counterparty`** — clients/vendors. `{id, name, kind: CUSTOMER|VENDOR|BOTH, contact…, is_active}`.
**Name is sensitive (D11)** — flagged for redaction were any cloud egress to be reintroduced; the v1
engine makes no cloud calls (§8.4), so the name stays local.

**`item`** — reusable dictionary templates (D14).
`{id, kind: SERVICE|EXPENSE, name, default_unit_price, default_account_id, unit_label, is_active}`.
Service items link to an **income** account; expense items to an **expense** account.

### 4.3 Invoices, bills, payments (accrual, D4)

**`invoice`** `{id, number, counterparty_id, issue_date, due_date, status: DRAFT|OPEN|PARTIAL|PAID|VOID, memo, currency, sync identity}`
**`invoice_line`** `{id, invoice_id, item_id?, description, qty, unit_price, line_total, account_id, is_tax}`
- A **tax line** (`is_tax = true`) posts to a **Sales Tax Payable (LIABILITY)** account (D15) — not income.
- **Issuing** an invoice posts a journal entry: **Dr Accounts Receivable**, **Cr Income** (per line),
  **Cr Sales Tax Payable** (tax lines). Drafts post nothing.

**`bill`** / **`bill_line`** — symmetric for AP (**Dr Expense**, **Cr Accounts Payable**).

**`payment`** `{id, date, amount, account_id (cash/bank), applies_to: invoice|bill, target_id, counterparty_id}`
- Receipt: **Dr Cash/Bank, Cr Accounts Receivable**; updates invoice status. The AR/AP leg is tagged
  with `counterparty_id` so each party's running balance is computable.
- Bill payment: **Dr Accounts Payable, Cr Cash/Bank**.

**Customer/vendor credit (D26).** Overpayment is **allowed by design** — a customer may pay more
than an invoice (or we may overpay a vendor). The excess is **available credit** tracked
**balance-forward** per counterparty: a counterparty's balance is the Σ of its AR (or AP) postings,
and a net-negative AR means the customer is in credit. Document settlement is an open-item
**`allocation`** ledger `{counterparty_id, source_kind: PAYMENT|CREDIT, payment_id?, target, amount}`:
a cash payment auto-allocates up to the document's remaining balance (overage → credit); existing
credit is applied to other open documents **manually** via `credit.apply`, which records an
allocation but posts **no cash and no journal entry** (the AR/AP already reflects the prepayment).
Invariants: `AR(cp) = docs_outstanding(cp) − available_credit(cp)`; the books always balance (Σ
postings = 0). Aging (`ar_aging`/`ap_aging`) measures outstanding as total − Σ allocations.

**Edit model (D13 — a document is editable only while DRAFT; corrections via new documents).** An
issued invoice/bill is a sent business document, so it is **immutable once issued** — you don't edit
it, you correct it with another document:
- **DRAFT** — fully editable (add/remove lines, change number/due date/memo); posts nothing.
- **OPEN / PARTIAL / PAID** — **locked**. The detail view hides the edit affordance.
- **Void** (`invoice.void` / `bill.void`) — an issued **OPEN (unpaid)** document can be cancelled: it
  posts a *reversing* journal entry that cancels AR/AP + income/expense, flags the original entry
  `REVERSED` (so status-filtered effective reports net it out), marks the document `VOID`, and drops
  it from aging. A **PARTIAL/PAID** document cannot be voided (cash has been applied) — correct it
  with a **refund**, a **credit note / discount on the next document**, or **customer/vendor credit**
  (D26). A DRAFT is not voided (it was never issued) — edit or discard it.

### 4.4 Local-only / non-synced tables

- **`tool_permission`** `{tool_name, policy: PERMIT|ASK|BLOCK}` — defaults: reads PERMIT, writes ASK (D18).
- **`dashboard_widget`** `{id, view_name, params(JSON), size: HERO|NORMAL, position}` (D17).
- **`app_setting`** `{k, v}` — generic local key/value (e.g. the onboarding flag).
- **`book_meta`** `{currency, device_id, …}`; schema version via `PRAGMA user_version`.
- *(Removed with the in-app agent: `pseudonym_map` for redaction egress and `provider_config` for LLM
  keys/models — the engine no longer makes cloud calls, so neither is needed.)*

### 4.5 Migrations

`PRAGMA user_version` read at startup; ordered migration steps each in a transaction; bump version
(RESEARCH §4). FKs (`PRAGMA foreign_keys=ON`) and `busy_timeout` set per connection.

---

## 5. Accounting semantics

- **Normal balances:** Assets/Expenses debit-normal; Liabilities/Equity/Income credit-normal.
- **Balance Sheet:** Assets = Liabilities + Equity (Equity includes retained earnings = cumulative
  Income − Expenses).
- **P&L:** Income − Expenses over a period.
- **Friendly vs advanced (D6):** friendly view groups asset/liability accounts as "Accounts" and
  income/expense as "Categories"; advanced reveals the five types and the full chart.
- **Corrections (D13):** never edit a posted entry. "Edit" in the UI = post a reversal
  (`status=REVERSAL`, `reverses_id`) + a new entry. "Delete" = reversal only. Advanced view shows history.

---

## 6. Reporting & dashboard

### 6.1 Reports as views (D16, D17)
Every report is a named, parameterized **view** → structured data + a display type
(`number` / `list` / `table` / `chart`). The same view result feeds the Reports section, dashboard
widgets, and the MCP `run_report` tool.

| View | Params | Display |
|---|---|---|
| `balance_sheet` | as-of date | table |
| `pnl` | date range | table + chart |
| `trial_balance` | as-of date | table |
| `general_ledger` | account_id, date range | table (running balance) |
| `transactions` (journal) | date range | table — **every recorded journal entry** (date, memo, source, status, amount); the audit view, includes reversals |
| `category_transactions` | type (`INCOME`/`EXPENSE`), date range | table — posting-level list of income or expense activity (date, memo, category, amount); only effective (`POSTED`) entries |
| `ar_aging` / `ap_aging` | as-of date, buckets | table |
| `cash_flow` | date range | table (v1: pragmatic cash-in/out over cash/bank accounts) |
| `cash_position` | — | number (default **hero** widget) |
| `outstanding_invoices` | — | list/number |
| `income_vs_expense` | period | chart |

**Drill-down (D17):** dashboard cards are click-through — Income/Expenses cards open
`category_transactions` filtered to that type; the Cash position / Net cash flow cards can open the
relevant ledger. The **Transactions** screen hosts `transactions` (All) with Income/Expense filters.

All reports support **date-range and account/category filtering**.

### 6.2 Dashboard (D17)
- Dashboard = ordered **widget instances**, each referencing a view + size (`HERO`/`NORMAL`) + params.
- v1 customization: **add/remove + reorder + size**; full drag-grid is a fast-follow.
- **Cash position** is the default hero widget. The Reports section always lists every view (nothing lost).
- **AI-native:** an MCP client can define a custom view and **pin it** as a widget.

---

## 7. UI

- **Shell:** WKWebView (`webview/webview`); assets in `Contents/Resources`; correct bundle id for
  Web Inspector debugging (RESEARCH §1).
- **Front-end stack (D24):** **React + Vite**, built to static assets bundled in the app. (Preact is
  a drop-in lighter fallback if bundle size matters.) Adds a Node/Vite build step alongside the C build.
- **C↔JS bridge:** JS calls `window.<op>(jsonArgs)` → engine op → JSON result; engine pushes updates
  via `eval`. Treated as tiny JSON-RPC; one JSON lib at the boundary.
- **First-run wizard (D7):** create book → choose **starter template** (freelancer chart of accounts,
  editable) or **start empty** → set currency.
- **Primary flows ("few buttons"):** record income, record expense, new invoice, mark invoice paid,
  new bill / pay bill, add account/category, add service/expense item, view reports.
- Screens: Dashboard · Transactions · Invoices · Bills · Accounts & Categories · Items · Reports.
- **MCP affordances (company launcher):** each company row has a **Connect to Claude** action that
  opens a modal with a ready-to-paste MCP config snippet (real binary/book/config paths via
  `app.mcp_info` — one server entry per company), and a **MCP tools window** listing the full exposed
  surface (Read vs Write + the approval note) so the user knows the capability *before* connecting a
  client. *(A top-level entry point for these is a possible fast-follow — today they're under ⇄ Switch
  company.)*
- **Detail views:** clicking an invoice/bill row opens a **detail view** (header + counterparty +
  line items + total + status + an Edit (DRAFT) / Void (issued, unpaid) affordance per the edit
  model above). Clicking a
  Dashboard Income/Expenses card opens the matching `category_transactions` list; the **Transactions**
  screen shows all journal entries (`transactions` view) with All/Income/Expense filters. Clicking an
  account in **Accounts & Categories** opens its **`general_ledger`** (Debit/Credit columns + running
  balance, shown in the account's natural sign).

---

## 8. AI & MCP

### 8.1 MCP server (RESEARCH §2)
- **Transport-agnostic core:** `mcp_handle_message(server, json_in) → json_out` over a tool registry.
- **Transports:** stdio (shim binary) + in-process Streamable HTTP/SSE on `127.0.0.1` (validate
  `Origin`, bind localhost). Target protocol rev **2025-11-25**.
- **Methods:** `initialize` (+ `notifications/initialized`), `ping`, `tools/list`, `tools/call`.
- **Single AI surface:** the MCP server is the *only* AI interface. The app makes no LLM calls of its
  own; an external MCP client (e.g. Claude Desktop) connects to the engine's tool registry.

### 8.2 Tool surface (intent-based, never raw CRUD — D8)
**Implemented: 23 tools** (each maps 1:1 to an `mb_api_dispatch` method — the same surface the UI
bridge uses), introspectable via `mb_mcp_tools_catalog` (the in-app "MCP tools" window, §7).
- **Reads (11, default Permit):** `list_accounts`, `get_account`, `list_counterparties`,
  `get_invoice`, `list_invoices`, `get_bill`, `list_bills`, `report_trial_balance`, `report_pnl`,
  `report_balance_sheet`, `report_cash_flow`.
- **Writes (12, default Ask + approval gate):** `create_account`, `record_income`, `record_expense`,
  `post_transaction`, `create_counterparty`, `create_invoice`, `add_invoice_line`, `issue_invoice`,
  `create_bill`, `add_bill_line`, `enter_bill`, `record_payment`.

Each tool has a JSON-Schema input; writes go through the engine (invariants hold; corrections via
reversal). Coverage: `tests/mcp_tools.c` drives **every tool through the JSON-RPC layer** end-to-end
(a rule going forward — a new tool ships with its integration test). *Not yet exposed (engine supports
them): items, dashboard, transactions journal, search — fast-follow additions.*

### 8.3 Permissions (D18)
Per-tool policy **Permit / Ask / Block** (Claude-Desktop-connector style). Enforced **server-side**
in the engine/MCP layer (covers every external MCP client). Factory defaults: reads =
Permit, writes = **Ask**, nothing Block.

**Write-approval gate (server-side, stronger than policy).** Independent of the policy above, *every*
write tool requires explicit user approval: called without `confirm: true` the server returns an
`approval_required` message and **writes nothing**; the client shows it to the user, who approves, and
the tool is re-called with `confirm: true`. This does not trust the client to prompt — even a client
that auto-approves still gets the gate. `tools/list` advertises the `confirm` param + a `[WRITE]` note.
BLOCK still hard-refuses; reads are never gated.

### 8.4 Sidebar agent (D9) — **removed**
An in-app sidebar agent (pluggable Anthropic/OpenAI/OpenRouter LLM client over libcurl, with a
redacting egress layer) was built in Phase 5 and then **descoped and deleted** — it made the app a
network client and duplicated what an external MCP client already does better. AI access is now
**solely** through the MCP server (§8.1): the user brings their own LLM client (e.g. Claude Desktop),
which connects to the engine's tool registry. The engine itself makes **no outbound network calls**.

---

## 9. Privacy & security (D10, D11)

- **Storage is 100% local, and the engine makes no outbound network calls.** With the in-app agent
  removed (§8.4), the app is fully offline; any data that leaves the device does so only through the
  user's **own external MCP client** (e.g. Claude Desktop), under that client's control and policy.
- **Minimization** — the intent-based tool surface returns aggregates/least-needed rows, not full
  ledgers, limiting what an MCP client can pull in one call.
- **Local transport** (future embedded HTTP/SSE) bound to loopback with `Origin` validation
  (DNS-rebinding defense); stdio transport talks only to the client that launched the shim.
- *(Historical: the deleted sidebar agent added a redacting egress choke point — pseudonymizing
  counterparty/account names before cloud calls. With no in-engine egress, that layer is gone; the
  responsibility for what reaches a provider now sits with the user's MCP client.)*

---

## 10. Persistence & ops

- One SQLite file = the book (and the portable export format; bit-identical across machines).
- **WAL + single-writer** (engine) + `busy_timeout` + `BEGIN IMMEDIATE` for write txns.
- **Backups:** SQLite Online Backup API; handle `-wal`/`-shm` sidecars. **DB location / backup UX (TODO).**
- **Export:** `.sqlite` file copy; CSV via SELECT loops. **Export scope/format UX (TODO).**
- **Attachments / receipts (TODO):** store file paths vs blobs vs out-of-scope-v1.

---

## 11. Quality: testing & error handling (D21 — built, PHASE0.md)

- **Tests start at commit #1.** Rust-style: in-file unit tests (`#ifdef MB_TEST`), integration tests
  in `tests/`, auto-registered, value-printing, `--json`/`--filter`. `make test` runs under ASan+UBSan.
- **Errors:** `mb_err` codes + located thread-local last-error + `MB_TRY` breadcrumbs;
  `MB_INVARIANT` (always-on integrity) vs `MB_DEBUG_ASSERT`; **`MB_MUST_CHECK` makes ignored errors a
  compile error.**
- **Dead code:** compile warnings + `MB_MUST_CHECK`, `make deadcode` (coverage), `make analyze`.
- **Convention:** every module ships embedded unit tests; every fallible fn returns `mb_err` +
  `MB_MUST_CHECK`; cross-module flows get an integration test.

---

## 12. Build & packaging

- **Build:** clang/C11; `make test|release|leaks|analyze|deadcode|clean` (Makefile). Apple `leaks`
  for memory (Valgrind/LSan weak on arm64).
- **Packaging (TODO detail):** `.app` bundle (`Contents/{MacOS,Info.plist,Resources}`), correct
  `CFBundleIdentifier`; Developer ID signing + Hardened Runtime + **notarize & staple** for Gatekeeper;
  build `arm64`. Two executables: the app and the stdio MCP shim.
- **CI (TODO):** run `make test` + `make analyze` on every change; periodic `make deadcode`.

---

## 13. P2P sync — **SUPERSEDED** (RESEARCH §5)

> **⚠ STALE (kept for the iroh research notes only).** This section describes the **abandoned**
> version-vector multi-device *sync* model. Phase 7 was reframed (2026-06-19) to **live read-only book
> sharing** (host serves a read-only view over iroh; a guest connects to view/export) and **shipped** —
> see the §14 Phase 7 entry and `~/.claude/plans/linked-booping-flame.md`. The iroh transport groundwork
> below (§5–6, §10–11) was reused; the version-vector sync model was not. Cross-platform builds (D27)
> are now Phase 6, in progress.

- **Design-now (D20):** every entry already carries `device_id`, `seq`, Lamport clock, hash chain.
- **Transport:** `iroh-c-ffi` — dial-by-public-key QUIC + NAT traversal + relay fallback.
- **Sync model:** exchange **version vectors** → ship missing immutable entries → union + deterministic
  order. **No CRDT needed** for the journal (append-only). msquic is a pure-C fallback transport.
- **VPS (TODO):** optional relay/discovery/metadata assist role to define later.

---

## 14. Roadmap (proposed)

- **Phase 0 — Foundation (DONE):** test harness, error model, build, `money` module.
- **Phase 1 — Engine core (CORE DONE, 2026-06-14):** SQLite store (open/migrate/WAL), accounts,
  journal entries + postings, balance invariant, reversing entries, sync-identity + SHA-256 hash
  chain. 23 tests green under ASan+UBSan. *Remaining: account list queries, seed-from-starter-chart.*
- **Phase 2 — Accounting features (DONE, 2026-06-15):** counterparties, items, invoices/bills
  (DRAFT→issue posting, tax→liability), payments (receipt/payment, PARTIAL→PAID), AR/AP. 41 tests
  green under ASan+UBSan, 0 leaks.
- **Phase 3 — Reporting (DONE, 2026-06-15):** view engine + Trial Balance, P&L, Balance Sheet,
  General Ledger, AR/AP aging, Cash Flow. 47 tests green, 0 leaks.
- **Phase 4 — UI (DONE, 2026-06-17):** JSON API bridge (`src/api`, `mb_api_dispatch` — shared
  with MCP), React/Vite front-end (`ui/`), native WKWebView shell (`src/app/main.c`, `make app`).
  Done: **Record** income/expense form; **Invoices & Bills** tab (list → **detail view** → edit;
  create draft with line items → issue/enter; record payments; **edit-while-draft, lock-on-issue,
  Void to cancel** (D13); **Transactions** tab (All journal w/ Income/Expense/Transfer type + accounting-style amounts;
  Income/Expense category lists; dashboard cards drill in); **per-account General Ledger** (Dr/Cr +
  running balance, off the Accounts list); **first-run wizard** (D7); **dashboard widget
  customization** (Cash position hero); **multi-company** (D25: registry + launcher + ⇄ switch).
  **Accounts & Categories editing** (create via friendly "what is it?", rename/recode, archive/restore;
  system accounts locked; `account.update`/`set_active`); **Items dictionary** (`src/item` + new
  `mb_item_list`; Items tab create/archive; **"+ Add from item"** autofills invoice/bill lines —
  description, qty, price, category — filtered by Service/Expense). ✅ verified live in preview each
  step; `make test` 76/76. **Phase 4 complete.**
- **Phase 5 — AI (MCP server DONE, 2026-06-15; in-app agent REMOVED + MCP hardened, 2026-06-19):**
  MCP server `src/mcp` (JSON-RPC 2.0, protocol 2025-11-25) — **23 intent tools** over `mb_api_dispatch`,
  per-tool Permit/Ask/Block (D18), stdio binary `make mcp` for Claude Desktop ([MCP.md](MCP.md)). **AI
  access is now solely via this MCP server** (the user brings their own LLM client). The in-app sidebar
  agent (`src/agent`/`src/llm`/`src/redact`/`src/secret`, `agent.send` + Assistant/Settings UI) was
  built and then **deleted** — it duplicated an external MCP client while turning the offline engine
  into a network client. Hardened since: **server-side write-approval gate** (every write needs
  `confirm:true`, §8.3); `list_invoices`/`list_bills`/`get_bill`; tool **catalog** (`mb_mcp_tools_catalog`)
  behind the in-app **MCP tools window** + per-company **Connect-to-Claude** modal (`app.mcp_info`, §7);
  **integration tests** drive every tool over JSON-RPC (`tests/mcp_tools.c`) — which caught a real
  `post_transaction` bug (uninitialized `counterparty_id`). 87 tests green, 0 leaks.
  *Remaining (optional polish): embedded HTTP/SSE MCP transport (today stdio only); a per-tool-policy
  settings screen; expose items/dashboard/journal/search tools.*
- **Phase 7 — Live read-only book sharing (DONE; reframed from P2P device sync):** the original
  version-vector multi-device sync was **replaced** by host→guest live sharing — the owner serves a
  read-only view of a book over `iroh-c-ffi` QUIC and a trusted party (e.g. an accountant) connects to
  view/export reports. Read-only enforced in 3 layers (UI → `mb_api_dispatch_guest` allowlist →
  `mb_store_open_readonly`). 7b-1 loopback protocol / 7b-2 real iroh transport / 7b-3 app+UI wiring
  (Stop revokes a live guest) all shipped. Brought in the Rust toolchain (`iroh-c-ffi`, lone Rust dep).
  *§13 below still describes the abandoned version-vector model — stale; see `~/.claude/plans/
  linked-booping-flame.md` for the live-sharing design that superseded it.*
- **▶ Phase 6 — Packaging + cross-platform (IN PROGRESS; target Windows first):** foundation landed &
  verified on macOS — SQLite **vendored** (drops system `-lsqlite3`), cross-platform **CMakeLists**
  (test/mcp/app), macOS host glue isolated behind `src/app/platform.h`, portable thread/libc shims
  (`src/support/mb_thread.*`, `mb_compat.h`) so the engine needs no pthreads/`<unistd.h>`. The
  Rust-free core (engine + tests + mcp) compiles first on Windows. Remaining: Windows data-dir branch,
  `platform_win.c` (dialogs/exe path), WebView2 + iroh Windows libs, NSOpenPanel/Win "open book",
  `.app`/MSI/AppImage signing/notarization, CMake Linux build (D27), CI.

---

## 15. Open items (remaining lighter clusters — to decide)

1. **Attachments/receipts** — paths vs blobs vs out-of-scope v1.
2. **DB location & backup UX** — where the file lives; backup cadence/UI; restore.
3. **Export** — formats/scope (CSV per table? whole `.sqlite`? PDF later).
4. **Packaging/signing** — Developer ID vs App Store sandbox; notarization pipeline specifics.
5. **P2P/VPS detail** — the VPS's exact role; pairing/identity UX.
6. **CI** — provider/runner; gates.
7. ~~**Starter chart of accounts**~~ → **DONE:** [STARTER_CHART.md](STARTER_CHART.md)
   (US freelancer / software-consulting template, Schedule C-aligned, with a JSON seed).

---

## 16. Decision index

D1 WKWebView · D2 freelance/single-book · D3 single-currency (ready) · D4 accrual+AR/AP · D5
invoices record-only (PDF/email-ready) · D6 double-entry + friendly/advanced · D7 first-run wizard ·
D8 single-writer engine + MCP topology · ~~D9 pluggable LLM providers~~ *(in-app agent removed
2026-06-19; AI is now external-MCP-client only — see §8.4)* · ~~D10 redact-egress posture~~ *(removed
with the agent; engine makes no cloud calls)* · D11 sensitive fields (names + account numbers) · D12
integer cents · D13 immutable journal · D14
item/template dictionaries · D15 tax-as-liability-line · D16 report suite · D17 widget dashboard ·
D18 per-tool Permit/Ask/Block · D19 validated tech stack · D20 sync-ready entry identity · D21
Phase 0 testing/error/build · ~~D22 portable secret store (Keychain now)~~ *(removed with the agent)*
· D23 portability principle
(Mac first, Linux/Windows later) · D24 front-end React + Vite · D25 multi-company = one file per
company + app-level registry + launcher (no in-DB multi-tenancy) · D26 customer/vendor credit =
overpayment allowed; per-counterparty AR/AP **balance-forward** truth (AR/AP postings tagged with
`counterparty_id`) + an `allocation` open-item ledger; available credit applied to open documents
**manually** (`credit.apply`, no cash / no journal entry). · D27 cross-platform = **one codebase,
compile-time platform selection** (`#ifdef` behind thin seams: paths, native shell, build), **not
separate forks**; per-OS differs only in build/packaging/signing, not source (§3.3). Sequencing:
**Phase 7 (P2P) before Phase 6 (packaging + cross-platform)**. Full text:
[PROJECT_NOTES.md](PROJECT_NOTES.md).
