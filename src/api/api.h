#ifndef MB_API_H
#define MB_API_H
/*
 * Money Books — JSON command API (Phase 4/5, SPEC §7/§8).
 *
 * One dispatch surface used by BOTH the WebView UI bridge and the MCP tool layer:
 * a method name + JSON args -> engine ops -> JSON result. All writes go through the
 * engine (invariants hold; D8). This is the contract the UI and the AI both speak.
 */
#include "../store/store.h"

/* Dispatch `method` with `args_json` (may be NULL/empty for no args).
 * *result_json is always set to a malloc'd JSON string (free with free()):
 *   success -> the result object;  failure -> {"error":{"code":...,"message":...}}.
 * Returns the underlying mb_err as well. */
mb_err mb_api_dispatch(mb_store *s, const char *method, const char *args_json,
                       char **result_json) MB_MUST_CHECK;

#endif /* MB_API_H */
