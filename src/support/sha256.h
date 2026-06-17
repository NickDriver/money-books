#ifndef MB_SHA256_H
#define MB_SHA256_H
/* Money Books — SHA-256 (public-domain style). Used for the journal hash chain (D20). */
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t  buf[64];
  size_t   buflen;
} mb_sha256;

void mb_sha256_init(mb_sha256 *c);
void mb_sha256_update(mb_sha256 *c, const void *data, size_t len);
void mb_sha256_final_hex(mb_sha256 *c, char out[65]);  /* 64 hex chars + NUL */

#endif /* MB_SHA256_H */
