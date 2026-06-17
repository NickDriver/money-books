# Money Books

**A local-first, AI-native, double-entry accounting app for macOS.**

Money Books keeps your books on *your* machine — a single SQLite file per company, a real
double-entry engine written in C, and an AI assistant that works over the exact same API as the
UI. No cloud account, no subscription, no telemetry. Your financial data never leaves your laptop
unless you ask it to.

> Status: early but real. The engine, accrual features, reporting, multi-company support, and the
> macOS app all work end-to-end. **82 tests pass under AddressSanitizer + UBSan with zero leaks.**

---

## Why it exists

Most accounting tools are either cloud SaaS (your books live on someone else's server) or heavy
desktop suites. Money Books is a third option:

- **Local-first.** One company = one `.sqlite` file you own, back up, and move around. Built so a
  future peer-to-peer sync layer can be added without re-architecting.
- **Correct by construction.** A genuine double-entry core: every transaction is a balanced set of
  signed postings that sum to zero, money is integer cents (no floats), and the journal is
  **immutable and append-only** — corrections are reversing entries, never edits-in-place.
- **AI-native, safely.** A built-in assistant and an MCP server both drive the same engine API.
  Client and account names are **redacted before anything is sent to a cloud model**, API keys
  live in the macOS Keychain (never in the database), and every write tool can be set to
  Permit / Ask / Block.
- **Friendly without dumbing down.** A plain "Accounts & Categories" lens sits on top of the pure
  double-entry ledger, so it reads like a normal money app but the books underneath are real.

---

## Features

- **Double-entry ledger** — balanced postings, integer cents, immutable journal with a
  tamper-evident hash chain and sync-ready entry identity.
- **Accrual accounting** — invoices, bills, accounts receivable/payable, payments.
- **Customer & vendor credit** — overpay an invoice or a bill and the excess becomes a
  per-counterparty credit balance (balance-forward via AR/AP) you can manually apply to future
  documents.
- **Edit-until-paid** — drafts edit freely; an issued-but-unpaid document can be reopened (which
  posts a reversing entry), edited, and re-issued; paid documents are locked.
- **Reports** — Profit & Loss, Balance Sheet, Cash Flow, Trial Balance, AR/AP Aging, per-account
  General Ledger, and a full journal/transactions view.
- **Multiple companies** — one SQLite file per company, with a launcher and a registry of your
  books; switch companies without restarting.
- **AI assistant** — bring-your-own-key (OpenAI / OpenRouter today), redaction before egress.
- **MCP server** — a stdio server so Claude Desktop (and other MCP clients) can work with your
  book through the same intent tools.

---

## Architecture

```
            ┌──────────────────────────┐
  React +   │   UI (WKWebView shell)    │     stdio MCP server  ◄── Claude Desktop, etc.
  Vite ───► │   window.mbInvoke(...)    │            │
            └────────────┬─────────────┘            │
                         │                           │
                  mb_api_dispatch(store, method, args_json)  ◄── one JSON contract
                         │
            ┌────────────▼─────────────┐
            │   C engine               │  accounts · journal · invoices · bills ·
            │   (pure C11, no GC)      │  payments · credit · reports · redaction · agent
            └────────────┬─────────────┘
                         │
                    SQLite (WAL, migrations)   ── one file = one company
```

- **Engine:** pure C11 (clang), the only writer, with a uniform `mb_err` error model and
  `#ifdef MB_TEST` unit tests living next to the code (Rust-style).
- **Storage:** SQLite with schema migrations via `user_version`.
- **UI:** React + Vite, bundled to a single inlined HTML and rendered in a native WKWebView via
  [webview/webview](https://github.com/webview/webview).
- **One API surface:** the UI bridge and the MCP server both call `mb_api_dispatch` — the same
  methods, the same validation, no second code path.

---

## Build & run (macOS, Apple Silicon)

Requirements: `clang`, system `sqlite3`, `libcurl`, and Node.js (for the UI). Apple Silicon is the
primary target; the code is written to be portable later.

```sh
# 1. run the test suite (ASan + UBSan)
make test

# 2. check for leaks (Apple `leaks`)
make leaks

# 3. build the MCP server for Claude Desktop
make mcp        # → build/money-books-mcp <book.sqlite>

# 4. build and run the macOS app
./scripts/fetch_webview.sh    # one-time: vendor the webview library
make app                      # builds the React UI + native shell
./build/MoneyBooks            # opens the company launcher
./build/MoneyBooks book.sqlite  # or open a specific book
```

The first launch opens a **launcher** where you can create a new company or open an existing book.

---

## Project layout

```
src/
  store/        SQLite open + migrations + sync stamps
  account/      chart of accounts (friendly Accounts/Categories lens)
  journal/      balanced, immutable postings + reversals + hash chain
  invoice/ bill/ counterparty/ item/   accrual documents
  payment/      payments + customer/vendor credit (allocations)
  report/       P&L, balance sheet, cash flow, trial balance, aging, ledger
  redact/       strip client/account names before cloud egress
  agent/ llm/   in-app AI assistant (provider-agnostic)
  secret/       API keys in the macOS Keychain
  api/          mb_api_dispatch — the single JSON contract
  app/ mcpd/    native WKWebView shell · stdio MCP server
  book/ registry/   multi-company: create/open/switch
ui/             React + Vite front-end
tests/          cross-module integration tests
docs/           SPEC, decision log, research notes
```

See [`docs/SPEC.md`](docs/SPEC.md) for the approved specification and
[`docs/PROJECT_NOTES.md`](docs/PROJECT_NOTES.md) for the decision log and build history.

---

## Design principles

- **Your data is yours.** Local-first, single-file books, no required network.
- **The books always balance.** Postings sum to zero; the journal is append-only; corrections are
  reversals. This is enforced by the engine and proven by tests.
- **AI is a tool, not a trust boundary.** Redact before egress, keys in the Keychain, per-tool
  permissions, and the assistant never moves money on its own.
- **Test-first.** Every fallible function returns an error code; new modules ship with unit tests;
  `make test` runs everything under sanitizers.

---

## Status & roadmap

Working today: the engine (accounts, journal, accrual), reporting, the JSON API, the React UI, the
native macOS app, the stdio MCP server, the in-app assistant, multi-company support, and
customer/vendor credit.

Planned next: in-app tool-confirmation UI, reply streaming, an Anthropic-native LLM dialect, an
embedded HTTP/SSE MCP transport, then packaging (signed `.app`) and peer-to-peer sync.

---

## License

Personal project, early stage — a license has not been chosen yet, so all rights are reserved for
now. Open an issue if you'd like to use it for something.
