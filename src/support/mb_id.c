#include "mb_id.h"

/* Cryptographic entropy, per platform. macOS & modern Linux expose getentropy(2);
 * Windows has no such header, so we use BCryptGenRandom (system-preferred RNG). */
#ifdef _WIN32
#  include "mb_win.h"   /* guarded <windows.h> (NOMB avoids the MB_OK collision) */
#  include <bcrypt.h>   /* link: bcrypt.lib */
static int mb_fill_entropy(unsigned char *b, size_t n) {
  return BCRYPT_SUCCESS(BCryptGenRandom(NULL, b, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? 0 : -1;
}
#else
#  include <sys/random.h>  /* getentropy — macOS & modern Linux */
static int mb_fill_entropy(unsigned char *b, size_t n) {
  return getentropy(b, n);
}
#endif

mb_err mb_uuid(char *out, size_t buflen) {
  if (!out || buflen < 37) return MB_FAIL(MB_ERR_INVALID_ARG, "uuid buffer too small");
  unsigned char b[16];
  if (mb_fill_entropy(b, sizeof b) != 0) return MB_FAIL(MB_ERR_INTERNAL, "entropy source failed");
  b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);  /* version 4 */
  b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);  /* variant   */
  static const char hex[] = "0123456789abcdef";
  size_t p = 0;
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[p++] = '-';
    out[p++] = hex[b[i] >> 4];
    out[p++] = hex[b[i] & 0x0F];
  }
  out[p] = '\0';
  return MB_OK;
}

#ifdef MB_TEST
#include "mb_test.h"
#include <string.h>

TEST(id, uuid_shape) {
  char a[40], b[40];
  ASSERT_OK(mb_uuid(a, sizeof a));
  ASSERT_OK(mb_uuid(b, sizeof b));
  ASSERT_EQ_INT((long)strlen(a), 36);
  EXPECT(a[8] == '-' && a[13] == '-' && a[18] == '-' && a[23] == '-');
  EXPECT(a[14] == '4');                 /* version nibble */
  ASSERT(strcmp(a, b) != 0);            /* not identical */
}

TEST(id, uuid_rejects_small_buffer) {
  char small[10];
  ASSERT_ERR(mb_uuid(small, sizeof small), MB_ERR_INVALID_ARG);
}
#endif
