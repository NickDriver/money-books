#ifndef MB_MCP_H
#define MB_MCP_H
/*
 * Money Books — MCP server core (Phase 5, SPEC §8, RESEARCH §2).
 *
 * Transport-agnostic JSON-RPC 2.0 handler. Exposes the mb_api_dispatch surface as
 * intent tools (initialize / ping / tools/list / tools/call), target protocol 2025-11-25.
 * Per-tool Permit/Ask/Block policy (D18); BLOCK hides/refuses a tool. For stdio (external
 * clients) Ask-confirmation is the client's job, so PERMIT and ASK are both callable.
 */
#include "../store/store.h"

/* Handle one JSON-RPC message. *out_json is set to a malloc'd response (free with free()),
 * or NULL for notifications (no reply is sent). Returns mb_err for transport-level issues. */
mb_err mb_mcp_handle(mb_store *s, const char *msg_json, char **out_json) MB_MUST_CHECK;

/* Settings: set a tool's policy ("PERMIT"/"ASK"/"BLOCK"). */
mb_err mb_mcp_set_policy(mb_store *s, const char *tool, const char *policy) MB_MUST_CHECK;

#endif /* MB_MCP_H */
