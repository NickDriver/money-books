#include "secret.h"

#include <stdio.h>
#include <string.h>

#if defined(__APPLE__) && !defined(MB_SECRET_MEMORY)
/* ---- macOS Keychain backend (Security.framework) ---- */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#define MB_KC_SERVICE CFSTR("MoneyBooks")

static CFMutableDictionaryRef base_query(const char *account) {
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(
      NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrService, MB_KC_SERVICE);
  CFStringRef acc = CFStringCreateWithCString(NULL, account, kCFStringEncodingUTF8);
  CFDictionarySetValue(q, kSecAttrAccount, acc);
  CFRelease(acc);
  return q;
}

mb_err mb_secret_set(const char *account, const char *value) {
  CFMutableDictionaryRef q = base_query(account);
  CFDataRef data = CFDataCreate(NULL, (const UInt8 *)value, (CFIndex)strlen(value));
  CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
      NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(attrs, kSecValueData, data);
  OSStatus st = SecItemUpdate(q, attrs);
  if (st == errSecItemNotFound) {
    CFDictionarySetValue(q, kSecValueData, data);
    st = SecItemAdd(q, NULL);
  }
  CFRelease(attrs);
  CFRelease(data);
  CFRelease(q);
  return st == errSecSuccess ? MB_OK : MB_FAIL(MB_ERR_INTERNAL, "keychain set failed (%d)", (int)st);
}

mb_err mb_secret_get(const char *account, char *buf, size_t buflen) {
  CFMutableDictionaryRef q = base_query(account);
  CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef out = NULL;
  OSStatus st = SecItemCopyMatching(q, &out);
  CFRelease(q);
  if (st == errSecItemNotFound) return MB_FAIL(MB_ERR_NOT_FOUND, "no secret for '%s'", account);
  if (st != errSecSuccess || !out) return MB_FAIL(MB_ERR_INTERNAL, "keychain get failed (%d)", (int)st);
  CFDataRef data = (CFDataRef)out;
  CFIndex len = CFDataGetLength(data);
  mb_err e = MB_OK;
  if ((size_t)len >= buflen) e = MB_FAIL(MB_ERR_INVALID_ARG, "secret buffer too small");
  else { memcpy(buf, CFDataGetBytePtr(data), (size_t)len); buf[len] = '\0'; }
  CFRelease(out);
  return e;
}

mb_err mb_secret_delete(const char *account) {
  CFMutableDictionaryRef q = base_query(account);
  SecItemDelete(q);
  CFRelease(q);
  return MB_OK;
}

#else
/* ---- in-memory backend (tests / headless) ---- */
#define MB_SECRET_SLOTS 32
#define MB_SECRET_VLEN  1024
static struct { char account[64]; char value[MB_SECRET_VLEN]; int used; } g_slots[MB_SECRET_SLOTS];

static int find(const char *account) {
  for (int i = 0; i < MB_SECRET_SLOTS; i++)
    if (g_slots[i].used && !strcmp(g_slots[i].account, account)) return i;
  return -1;
}

mb_err mb_secret_set(const char *account, const char *value) {
  if (strlen(value) >= MB_SECRET_VLEN) return MB_FAIL(MB_ERR_INVALID_ARG, "secret too long");
  int i = find(account);
  if (i < 0) for (int j = 0; j < MB_SECRET_SLOTS && i < 0; j++) if (!g_slots[j].used) i = j;
  if (i < 0) return MB_FAIL(MB_ERR_INTERNAL, "secret store full");
  snprintf(g_slots[i].account, sizeof g_slots[i].account, "%s", account);
  snprintf(g_slots[i].value, sizeof g_slots[i].value, "%s", value);
  g_slots[i].used = 1;
  return MB_OK;
}

mb_err mb_secret_get(const char *account, char *buf, size_t buflen) {
  int i = find(account);
  if (i < 0) return MB_FAIL(MB_ERR_NOT_FOUND, "no secret for '%s'", account);
  if (strlen(g_slots[i].value) >= buflen) return MB_FAIL(MB_ERR_INVALID_ARG, "secret buffer too small");
  snprintf(buf, buflen, "%s", g_slots[i].value);
  return MB_OK;
}

mb_err mb_secret_delete(const char *account) {
  int i = find(account);
  if (i >= 0) { memset(&g_slots[i], 0, sizeof g_slots[i]); }
  return MB_OK;
}
#endif

int mb_secret_has(const char *account) {
  char tmp[2048];   /* large enough for any provider key */
  return mb_secret_get(account, tmp, sizeof tmp) == MB_OK;
}

#ifdef MB_TEST
#include "../support/mb_test.h"

TEST(secret, set_get_has_delete) {
  ASSERT_EQ_INT(mb_secret_has("llm:test"), 0);
  ASSERT_OK(mb_secret_set("llm:test", "sk-secret-123"));
  ASSERT_EQ_INT(mb_secret_has("llm:test"), 1);
  char buf[64];
  ASSERT_OK(mb_secret_get("llm:test", buf, sizeof buf));
  ASSERT_STR_EQ(buf, "sk-secret-123");
  ASSERT_OK(mb_secret_set("llm:test", "sk-updated"));   /* overwrite */
  ASSERT_OK(mb_secret_get("llm:test", buf, sizeof buf));
  ASSERT_STR_EQ(buf, "sk-updated");
  ASSERT_OK(mb_secret_delete("llm:test"));
  ASSERT_EQ_INT(mb_secret_has("llm:test"), 0);
  ASSERT_ERR(mb_secret_get("llm:test", buf, sizeof buf), MB_ERR_NOT_FOUND);
}
#endif
