# Money Books — MCP server (Phase 5)

The MCP server exposes the engine's vetted operations as tools an LLM can call. It speaks
**JSON-RPC 2.0** (protocol `2025-11-25`) and, by SPEC D8, drives the **single-writer engine** —
the AI uses the exact same operations the UI does, so it can't put the books in an invalid state.

Built from scratch in C ([src/mcp/mcp.c](../src/mcp/mcp.c)) per [RESEARCH.md](RESEARCH.md) §2.

## Build & run

```sh
make mcp                                  # builds build/money-books-mcp (pure C, no webview)
./build/money-books-mcp path/to/book.sqlite   # serves that book over stdio
```

First run on an empty book seeds the freelancer starter chart (so AR/AP/tax accounts exist).

## Connect Claude Desktop

Add to `~/Library/Application Support/Claude/claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "money-books": {
      "command": "/ABSOLUTE/PATH/money_books/build/money-books-mcp",
      "args": ["/ABSOLUTE/PATH/money_books/book.sqlite"]
    }
  }
}
```

Restart Claude Desktop; "money-books" appears as a tool source. Then ask things like
*"what's my P&L this quarter?"* or *"record $2,500 of consulting income deposited to checking."*

## Tools (23)

Each maps 1:1 to an `mb_api_dispatch` method (the same surface the UI bridge uses):

- **Read:** `list_accounts`, `get_account`, `list_counterparties`, `get_invoice`, `list_invoices`,
  `get_bill`, `list_bills`, `report_trial_balance`, `report_pnl`, `report_balance_sheet`,
  `report_cash_flow`
- **Write:** `create_account`, `record_income`, `record_expense`, `post_transaction`,
  `create_counterparty`, `create_invoice`, `add_invoice_line`, `issue_invoice`,
  `create_bill`, `add_bill_line`, `enter_bill`, `record_payment`

## Approval gate for writes (server-side)

**Every write tool requires explicit user approval, enforced by the server — independent of the
permission policy below.** A write tool called *without* `confirm: true` does **not execute**:
it returns an `APPROVAL REQUIRED` message (`structuredContent.approval_required = true`) describing
the pending change and writes nothing. The client must show it to the user, and only after approval
re-call the tool with `confirm: true`. Reads are never gated.

This does not rely on the client to prompt: even a client that auto-approves still gets the
approval request on the first call. `tools/list` advertises the contract — write tools carry a
`confirm` boolean in their `inputSchema` and a `[WRITE — requires user approval]` note.

## Permissions (D18)

Each tool also has a policy in the `tool_permission` table — **PERMIT / ASK / BLOCK** (orthogonal to,
and weaker than, the approval gate above):
- Defaults: reads → PERMIT, writes → ASK (nothing BLOCK).
- **BLOCK** hides the tool from `tools/list` and refuses `tools/call` (checked before the gate).
- PERMIT/ASK are both callable, but writes still hit the approval gate regardless.
- Set a policy programmatically: `mb_mcp_set_policy(store, "record_expense", "BLOCK")`. (A settings
  UI for this is future work.)

## Status & roadmap

- **Done:** stdio transport (what Claude Desktop uses), all 20 tools, permission policy. 6 MCP unit
  tests + a stdio smoke test, 0 leaks.
- **Future:** embedded local HTTP/SSE transport (for HTTP-based MCP clients) via civetweb; a settings
  screen for per-tool policy.
- **Removed (2026-06-19):** the in-app sidebar agent (pluggable Anthropic/OpenAI/OpenRouter LLM client
  + redacting egress, D9/D10). AI access is now solely through this MCP server — the user brings their
  own LLM client (e.g. Claude Desktop). The engine makes no outbound network calls.
