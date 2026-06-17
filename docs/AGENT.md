# Money Books — in-app AI assistant (Phase 5)

The **Assistant** tab is an AI agent that operates on your books through the same vetted engine
operations the UI uses (so it can't unbalance the books), with **sensitive names redacted before
anything leaves your machine** (D10/D11).

## Architecture

```
Assistant UI → agent.send (bridge) → mb_agent_run ──┐ tool-use loop
                                                     ├─ redact (Client_N / Account_N)  → LLM provider (libcurl)
                                                     ├─ execute tool → mb_api_dispatch → engine → SQLite
                                                     └─ restore tokens in the final reply
```

- **Provider-agnostic loop** ([src/agent](../src/agent/agent.c)) — tested with a mock.
- **Redaction egress** ([src/redact](../src/redact/redact.c)) — counterparty + money-account names
  become tokens before egress; categories/amounts pass through; tokens restored in replies. Tested.
- **Real provider** ([src/llm](../src/llm/llm.c)) — OpenAI-compatible dialect (covers **OpenAI** and
  **OpenRouter**) over libcurl. Anthropic-native dialect is a future addition (use OpenRouter for
  Claude models meanwhile).

## Configure (Settings UI — keys in the Keychain, D22)

Run the app, open the **Settings** tab, pick a provider (OpenAI or OpenRouter), paste your API key,
optionally set the model, click **Make active** + **Save**. The key is stored in the **macOS
Keychain** via `mb_secret_store` — never in the book file, logs, or egress. Then open **Assistant**
and chat ("what's my P&L this year?" / "record $2,500 consulting income to checking").

```sh
./build/MoneyBooks book.sqlite      # no env vars needed; configure the key in Settings
```

**Env-var override (headless/CI only, D22):** if no Keychain key is set for the active provider,
the agent falls back to `MB_LLM_API_KEY` (+ optional `MB_LLM_BASE_URL`, `MB_LLM_MODEL`). The
Settings UI is the normal path.

Settings API (also available to the MCP/agent surface): `settings.list_providers` (returns
`has_key` flags, never the key), `settings.set_provider {provider, api_key?, model?, base_url?,
active?}`, `settings.clear_key {provider}`.

## Privacy

Every message and every tool result is passed through the redactor before it reaches the provider:
client/vendor names → `Client_N`/`Vendor_N`, bank/cash/card account names → `Account_N`. Income/
expense **categories**, dates, and **amounts** pass through so the agent can actually reason and
compute. Provider notes (RESEARCH §3): OpenAI/Anthropic API don't train on API data by default;
**OpenRouter is a proxy** — prefer it less for sensitive content.

## Known limitations (v1)

- The agent call runs on a **worker thread** (the UI stays responsive; the chat shows an animated
  typing indicator). **Streaming** of the reply token-by-token is still future work.
- Anthropic **native** dialect not yet implemented (OpenRouter covers Claude models today).
- Per-tool Permit/Ask/Block is enforced for the MCP path; the in-app agent currently runs all its
  tools (a confirmation UI for "Ask" tools is future work).
