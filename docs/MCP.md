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

## Tools (20)

Each maps 1:1 to an `mb_api_dispatch` method (the same surface the UI bridge uses):

- **Read:** `list_accounts`, `get_account`, `list_counterparties`, `get_invoice`,
  `report_trial_balance`, `report_pnl`, `report_balance_sheet`, `report_cash_flow`
- **Write:** `create_account`, `record_income`, `record_expense`, `post_transaction`,
  `create_counterparty`, `create_invoice`, `add_invoice_line`, `issue_invoice`,
  `create_bill`, `add_bill_line`, `enter_bill`, `record_payment`

## Permissions (D18)

Each tool has a policy in the `tool_permission` table — **PERMIT / ASK / BLOCK**:
- Defaults: reads → PERMIT, writes → ASK (nothing BLOCK).
- **BLOCK** hides the tool from `tools/list` and refuses `tools/call` (server-side, enforced here).
- Over stdio, **ASK confirmation is the MCP client's responsibility** (e.g. Claude Desktop prompts
  before a tool call), so PERMIT and ASK are both callable; only BLOCK is refused server-side.
- Set a policy programmatically: `mb_mcp_set_policy(store, "record_expense", "BLOCK")`. (A settings
  UI for this is future work.)

## Status & roadmap

- **Done:** stdio transport (what Claude Desktop uses), all 20 tools, permission policy. 6 MCP unit
  tests + a stdio smoke test, 0 leaks.
- **Future:** embedded local HTTP/SSE transport (for the in-app agent + HTTP clients) via civetweb;
  the in-app sidebar agent (pluggable Anthropic/OpenAI/OpenRouter, D9) with redaction at the egress
  choke point (D10/D11); a settings screen for per-tool policy.
