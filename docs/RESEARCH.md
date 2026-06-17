# Money Books — Technical Research Findings

> Research pass to de-risk the pure-C architecture before writing the spec.
> Date: 2026-06-13. All findings verified against authoritative sources (see citations per section).
> **Headline: every high-risk unknown is feasible in pure C, with permissively-licensed building blocks.**

---

## TL;DR — recommended tech stack (all license-clean for a proprietary app)

| Concern | Choice | License | Confidence |
|---|---|---|---|
| Native web UI | **`webview/webview`** (C API → system WKWebView) | MIT | High |
| UI escape hatch | raw `objc_msgSend` for native menus/dialogs | system | High |
| Local store | **SQLite amalgamation** (`sqlite3.c/.h`), WAL mode | Public domain | High |
| JSON (everywhere) | **yyjson** (fast) or **cJSON** (simplest) | MIT | High |
| HTTP client (LLM APIs) | **libcurl** | curl (MIT-style) | High |
| Embedded HTTP/SSE server (MCP) | **civetweb** (NOT mongoose — GPL) | MIT | High |
| MCP protocol | **build from scratch** (JSON-RPC 2.0); target rev **2025-11-25** | — | High |
| P2P transport (future) | **`iroh-c-ffi`** (QUIC, dial-by-key, NAT traversal) | Apache-2.0/MIT | Medium |
| Sync model (future) | **append-only log + version vectors** (no CRDT needed) | — | High |
| P2P fallback (future) | **msquic** (pure C) if avoiding a Rust toolchain | MIT | Medium |

**One build implication:** everything is pure C **except** the future P2P phase, which links a Rust staticlib (`iroh-c-ffi`). The core app, engine, UI, MCP server, and LLM layer need **no Rust**. A Rust toolchain only enters the build when/if we add iroh.

---

## 1. Native web UI — "Tauri for C" via WKWebView

**Verdict: fully feasible today.** A pure-C app hosts an HTML/CSS/JS UI in the system WKWebView with two-way native↔JS calls; no Chromium, nothing bundled.

- **Library: `webview/webview`** (https://github.com/webview/webview) — MIT, ~14.1k stars, active. C++ internally but ships a **stable pure-C API** in `webview.h` (with a C example). On macOS it creates a real **WKWebView in an NSWindow via Cocoa** (system WebKit; no bundled engine). Ships via git tags/master, **no GitHub Releases → pin a commit/tag**.
- **Feature coverage (all confirmed):** load local HTML (`webview_set_html` / `webview_navigate` to `file://` or localhost), C→JS (`webview_eval`), JS→C (`webview_bind` + `webview_return`), window mgmt (`webview_set_title/size`), thread marshaling (`webview_dispatch`).
- **C↔JS bridge pattern:** treat it as tiny JSON-RPC. JS calls `window.foo(args)` → arrives in C callback as a **JSON string** → C validates + writes via the engine → returns JSON → JS updates view (or push via `webview_eval`). Calls are async. → Use a C JSON lib at the boundary.
- **Escape hatch:** call the Obj-C runtime directly (`objc_msgSend`) for native menus, file dialogs, dock, traffic-light styling. **arm64 trap:** you MUST cast `objc_msgSend` to the correct function-pointer signature per call (wrong cast silently corrupts on Apple Silicon). Use surgically — don't hand-roll the whole window.
- **Alternatives:** `saucer` (C++ w/ C bindings, MIT) — viable runner-up. `Ultralight` — **rejected**: proprietary non-system renderer, ~$3k/yr, defeats the "ship nothing" goal.
- **Limitations vs Chromium:** no built-in DevTools (debug via **Safari Web Inspector**; requires correct bundle ID + `inspectable=YES`); WebKit feature timeline (no WebGPU in WKWebView, lower memory ceiling, scheme-handler quirks). **None block a forms/tables/charts accounting UI.**
- **Shipping:** proper `.app` bundle (`Contents/{MacOS,Info.plist,Resources}`) — **the bundle + correct `CFBundleIdentifier` matters** for WebView focus/activation/devtools. Sign with Developer ID + Hardened Runtime, **notarize + staple** for Gatekeeper. Build `arm64` (or universal).
- **Effort/risk:** working bridged window in hours–1 day; production polish (menus, dialogs, signing pipeline) medium. Risk low–moderate.

Citations: github.com/webview/webview · github.com/saucer/saucer · ultralig.ht/pricing · developer.apple.com forums (Web Inspector #4326, WebGPU #770862).

---

## 2. MCP server in pure C

**Verdict: feasible and not large.** No mature pure-C SDK exists → build from scratch: a JSON-RPC 2.0 dispatcher (a handful of methods) over stdio, plus an optional small embedded HTTP/SSE server.

- **Protocol facts (verified):** JSON-RPC 2.0; latest stable revision **`2025-11-25`** (accept `2025-06-18` for compat). Two standard transports:
  - **stdio** — subprocess; newline-delimited JSON on stdin/stdout (**no embedded newlines**); stderr for logs. Clients (incl. Claude Desktop) prefer this. Simplest, build first.
  - **Streamable HTTP** — single endpoint, POST+GET, `Accept: application/json, text/event-stream`; optional `Mcp-Session-Id`; `MCP-Protocol-Version` header; **must validate `Origin` + bind to 127.0.0.1** (DNS-rebinding defense). Replaced the old HTTP+SSE transport.
  - (A future RC `2026-07-28` makes HTTP tool calls stateless — keep transport behind an interface so the JSON-RPC core survives the migration.)
- **Minimal method set:** `initialize` (+ `notifications/initialized`), `ping`, `tools/list`, `tools/call`. Declare `capabilities.tools`. Everything else (resources, prompts, sampling, auth) optional → omit for a tools-only server. Two error channels: JSON-RPC errors vs tool result `isError:true`.
- **Existing libs:** **no mature pure-C SDK** (`micl2e2/mcpc` is early/experimental). If C++-behind-a-C-ABI is acceptable, `GopherSecurity/gopher-mcp` (Apache-2.0, exposes C FFI) is the one off-the-shelf candidate. Otherwise build it.
- **Building blocks:** JSON = **yyjson** or **cJSON** (MIT). HTTP/SSE = **civetweb** (MIT). **Avoid `mongoose` — GPLv2/commercial dual license**, contaminates a proprietary app.
- **Effort:** stdio tools-only ≈ a few hundred–1k LOC, days. +HTTP/SSE ≈ another 1–2k LOC; tricky parts = SSE framing, session handling, capability/version negotiation, Origin security, thread-safety of the tool registry.
- **Recommended architecture:** a transport-agnostic core `mcp_handle_message(server, json_in) -> json_out` over a tool registry (name → {schema, handler}). Adapters: (a) **stdio** compiled into the thin **proxy shim binary** Claude Desktop launches — shim forwards to the live app over local IPC so tool state isn't duplicated; (b) **in-process HTTP/SSE** server for the in-app agent + HTTP clients. **In-app agent calls the core/registry directly in-process** → guaranteed parity with external clients.

Citations: modelcontextprotocol.io spec (transports/lifecycle/tools, 2025-06-18 & 2025-11-25 changelog) · github.com/modelcontextprotocol · micl2e2/mcpc · GopherSecurity/gopher-mcp · yyjson/cJSON · civetweb · mongoose.ws/licensing.

---

## 3. LLM provider APIs from pure C

**Verdict: fully viable.** libcurl (HTTP) + yyjson/cJSON (JSON) + a hand-written provider layer. Standard, battle-tested.

- **HTTP:** **libcurl** (curl license, MIT-style, ships on macOS). Build headers with `curl_slist` (`Authorization: Bearer` for OpenAI/OpenRouter; `x-api-key` + `anthropic-version: 2023-06-01` for Anthropic), POST JSON body, capture via `CURLOPT_WRITEFUNCTION`.
- **Streaming (SSE):** set `"stream": true`; libcurl delivers arbitrary-size chunks to the write callback — **you must buffer and split on `\n\n`**, strip `data: `, JSON-parse each event. `curl_easy_perform` blocks → run on a worker thread (or `curl_multi`). **OpenAI/OpenRouter** = `chat.completion.chunk` deltas ending in `data: [DONE]`; **Anthropic** = typed events (`content_block_delta`, etc.) → per-provider event decoder.
- **Tool/function-calling loop:** confirmed for all three. Crucially, **two schema dialects, not three**: OpenAI-style covers **OpenAI + OpenRouter** (OpenRouter is OpenAI-compatible and normalizes tool calls to the OpenAI schema, even for Anthropic models behind it); Anthropic-style is separate. The state machine (append assistant tool request → execute → append tool result → resend) is structurally identical.
- **JSON:** **yyjson** (MIT, fastest, single-file, has a builder API) recommended; **cJSON** (MIT) the simpler fallback. Payloads are small so speed rarely matters.
- **Effort:** moderate, low-risk plumbing. Non-streaming ~1–2d; SSE parser ~2–4d (the trickiest); tool-use loop ~2–3d; pluggable abstraction ~2–3d. Risks: SSE reassembly edge cases, schema drift (pin versions, isolate per-provider serialization), 429/overloaded retry+backoff, manual memory hygiene.

Citations: curl.se/libcurl (httpcustomheader, CURLOPT_WRITEFUNCTION, SSE mail thread) · platform.claude.com tool-use docs · developers.openai.com function-calling · openrouter.ai docs (OpenAI-compatible, tool calling) · yyjson.

---

## 4. SQLite from pure C

**Verdict: ideal fit.** Public-domain amalgamation; WAL + single-writer discipline cleanly supports the D8 topology (one engine writer, concurrent readers, a separate shim process).

- **Embed:** the **amalgamation** (`sqlite3.c` + `sqlite3.h`) is the officially recommended method; **public domain** (no attribution); single-TU build is also ~5–10% faster.
- **Concurrency (WAL):** `PRAGMA journal_mode=WAL` → many concurrent readers + exactly one writer; cross-process on the **same host** is supported (good for the shim auto-launching the engine). Best practices: `PRAGMA busy_timeout=5000` on **every** connection; **`BEGIN IMMEDIATE`** for write txns (takes the write lock up front, avoids the deferred→upgrade `SQLITE_BUSY` deadlock); keep a small commit-retry wrapper; funnel **all writes through one path** (matches D8/D13).
- **Features (in amalgamation):** transactions (wrap a balanced posting in `BEGIN IMMEDIATE…COMMIT` — atomic double-entry); foreign keys (**off by default** → `PRAGMA foreign_keys=ON` per connection, or `-DSQLITE_DEFAULT_FOREIGN_KEYS=1`); prepared statements (always); **FTS5** for memo search (build with `-DSQLITE_ENABLE_FTS5`); **JSON functions on by default** since 3.38. Suggested flags: `-DSQLITE_ENABLE_FTS5 -DSQLITE_DEFAULT_FOREIGN_KEYS=1 -DSQLITE_THREADSAFE=1`.
- **Backup/export:** **Online Backup API** (`sqlite3_backup_*`) snapshots a live DB without long locks; CSV via app-side `SELECT` loop; **the `.sqlite` file IS the portable format** — bit-identical across architectures, stable since 2004. Great as the export/interchange artifact.
- **Migrations:** **`PRAGMA user_version`** — read on startup, run ordered migration steps (each in a txn), bump the version. No extra tables.
- **Risks (minimal):** never on a networked filesystem; handle `-wal`/`-shm` sidecars in backup/move (use the backup API or checkpoint+copy); FKs and busy_timeout are **per-connection**.

Citations: sqlite.org (amalgamation, license, wal, compile options, backup, onefile/formatchng) · berthub.eu busy_timeout article.

---

## 5. Future P2P sync (iroh) — validates the immutable-journal design

**Verdict: realistic for a later phase.** Official C FFI exists for iroh's core (QUIC); build our own append-only-log sync on top.

- **`iroh-c-ffi`** (https://github.com/n0-computer/iroh-c-ffi) — **the C path**: ships `irohnet.h` + cdylib/staticlib, working C examples, Apache-2.0/MIT, active (v0.100.0 May 2026, tracks iroh 1.0.0-rc.1). Exposes `Endpoint`/`Connection`/`SendStream`/`RecvStream` — i.e. **authenticated, NAT-traversed QUIC streams keyed by node public key**. (Distinct from `iroh-ffi`, which is UniFFI for Python/Swift/Kotlin/Node — not C.)
- **Scope:** iroh-c-ffi is the **connection/transport layer only**. `iroh-blobs`, `iroh-gossip`, and `iroh-docs` (the syncable doc layer, now spun out) are **Rust-only** → don't depend on them from C; implement our own sync protocol over QUIC streams.
- **No CRDT needed for the journal.** An append-only, immutable, hash-chained **event log** suffices:
  - Each device owns a log keyed `(device_id, seq)`; `device_id` = stable id (e.g. iroh node pubkey); entries carry a Lamport clock + prev-hash (tamper-evident).
  - **Sync = exchange version vectors** (`{device_id → max_seq}`) → ship missing entries. Conflict-free **by construction** (entries never mutate/delete).
  - **Merge = union of logs**, deterministically ordered (Lamport, tie-break device_id); balances are a fold over the merged log → all devices converge.
  - **Corrections = new reversing entries** (already our D13) — fits perfectly.
  - A real CRDT (automerge-c / yffi, both MIT C APIs) is only needed if we later add concurrent *mutable* shared-field editing (e.g. two devices renaming one account) — not the core journal.
- **Effort/risk:** low–moderate; main costs are carrying a Rust toolchain for one staticlib + writing the sync protocol; risk = pre-1.0 API churn (pin version). **Fallback:** the same log-sync protocol runs over **msquic** (pure C, MIT) if we want to avoid Rust — losing hole-punching/relay (would need a rendezvous). → Keep the **sync protocol transport-agnostic**.
- **Design-now consequence:** make the storage/log format **sync-ready from day one** — `(device_id, seq)`, Lamport clock, content hash, prev-hash — even before any networking. This is the expensive-to-retrofit decision, and it's free to bake in now.

Citations: github.com/n0-computer/iroh-c-ffi (+commits) · iroh.computer/blog (ffi-updates, 0.28 crates) · n0-computer/iroh-docs · automerge-c · y-crdt/yffi · microsoft/msquic.

---

## Decisions this research confirms or informs

- **Confirms D1** (WKWebView via `webview/webview`) and **D8** (single-writer; WAL + `BEGIN IMMEDIATE` + cross-process shim is exactly the supported model).
- **Confirms D13** (immutable journal) is also the *right substrate for P2P sync* — append-only log + version vectors, no CRDT.
- **Informs D9** (providers): implement **two dialects** (OpenAI≈OpenRouter, Anthropic), not three.
- **New design-now constraint:** bake **sync-ready identity into every entry** (`device_id`, `seq`, Lamport clock, content+prev hash) from v1, even though P2P ships later.
- **Build/licensing:** standardize on **SQLite (PD) + yyjson/cJSON (MIT) + libcurl (MIT-ish) + civetweb (MIT) + webview (MIT)**; **avoid mongoose (GPL)**; Rust toolchain only at the iroh phase.
