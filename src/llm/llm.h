#ifndef MB_LLM_H
#define MB_LLM_H
/*
 * Money Books — real LLM provider adapters (Phase 5, D9). Translate the agent's
 * provider-agnostic protocol to/from a vendor API over libcurl.
 *
 * OpenAI-dialect covers OpenAI and OpenRouter (OpenRouter is OpenAI-compatible — RESEARCH §3).
 * Anthropic-dialect is a future addition. Network/key-dependent: build-verified, runs with a key.
 */
#include "../agent/agent.h"

/* Build an OpenAI-compatible provider. base_url e.g. "https://api.openai.com/v1" or
 * "https://openrouter.ai/api/v1". Strings are copied. Free with mb_llm_provider_free. */
mb_err mb_llm_openai_provider(const char *base_url, const char *api_key, const char *model,
                              mb_llm_provider *out) MB_MUST_CHECK;

/* Build a provider from environment: MB_LLM_BASE_URL, MB_LLM_API_KEY, MB_LLM_MODEL.
 * Returns MB_ERR_PERMISSION if no API key is configured. */
mb_err mb_llm_provider_from_env(mb_llm_provider *out) MB_MUST_CHECK;

void mb_llm_provider_free(mb_llm_provider *p);

#endif /* MB_LLM_H */
