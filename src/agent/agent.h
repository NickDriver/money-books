#ifndef MB_AGENT_H
#define MB_AGENT_H
/*
 * Money Books — sidebar agent orchestration (Phase 5, D9/D10/D11).
 *
 * Provider-agnostic tool-use loop. The provider is injectable (function pointer) so the
 * loop is testable with a mock and the real Anthropic/OpenAI/OpenRouter adapters (libcurl)
 * plug in later. All text crossing to the provider is redacted (egress choke point), and
 * tokens are restored in the final reply.
 *
 * Provider protocol (provider-agnostic JSON the adapter translates to/from its API):
 *   request : {"messages":[{role,content|tool_calls}], "tools":[{name,description}]}
 *   response: {"tool_calls":[{"id","name","arguments":{...}}]}  OR  {"text":"..."}
 */
#include "../store/store.h"

typedef char *(*mb_llm_complete_fn)(void *ctx, const char *request_json);  /* returns malloc'd JSON, or NULL */

typedef struct {
  mb_llm_complete_fn complete;
  void              *ctx;
} mb_llm_provider;

/* Run the agent for one user message; *reply_out is the final assistant text (malloc'd). */
mb_err mb_agent_run(mb_store *s, const mb_llm_provider *prov, const char *user_msg,
                    char **reply_out) MB_MUST_CHECK;

#endif /* MB_AGENT_H */
