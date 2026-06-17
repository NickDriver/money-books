#include "sha256.h"

#include <string.h>

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static void transform(mb_sha256 *c, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
           (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROTR(w[i-15],7) ^ ROTR(w[i-15],18) ^ (w[i-15] >> 3);
    uint32_t s1 = ROTR(w[i-2],17) ^ ROTR(w[i-2],19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  uint32_t a=c->state[0],b=c->state[1],cc=c->state[2],d=c->state[3],
           e=c->state[4],f=c->state[5],g=c->state[6],h=c->state[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ROTR(e,6) ^ ROTR(e,11) ^ ROTR(e,25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = ROTR(a,2) ^ ROTR(a,13) ^ ROTR(a,22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
  }
  c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=d;
  c->state[4]+=e; c->state[5]+=f; c->state[6]+=g; c->state[7]+=h;
}

void mb_sha256_init(mb_sha256 *c) {
  c->state[0]=0x6a09e667; c->state[1]=0xbb67ae85; c->state[2]=0x3c6ef372; c->state[3]=0xa54ff53a;
  c->state[4]=0x510e527f; c->state[5]=0x9b05688c; c->state[6]=0x1f83d9ab; c->state[7]=0x5be0cd19;
  c->bitlen = 0;
  c->buflen = 0;
}

void mb_sha256_update(mb_sha256 *c, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    c->buf[c->buflen++] = p[i];
    if (c->buflen == 64) { transform(c, c->buf); c->bitlen += 512; c->buflen = 0; }
  }
}

void mb_sha256_final_hex(mb_sha256 *c, char out[65]) {
  uint64_t bits = c->bitlen + (uint64_t)c->buflen * 8;
  c->buf[c->buflen++] = 0x80;
  if (c->buflen > 56) {
    while (c->buflen < 64) c->buf[c->buflen++] = 0;
    transform(c, c->buf);
    c->buflen = 0;
  }
  while (c->buflen < 56) c->buf[c->buflen++] = 0;
  for (int i = 7; i >= 0; i--) c->buf[c->buflen++] = (uint8_t)(bits >> (i*8));
  transform(c, c->buf);

  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++)
    for (int j = 3; j >= 0; j--) {
      uint8_t byte = (uint8_t)(c->state[i] >> (j*8));
      *out++ = hex[byte >> 4];
      *out++ = hex[byte & 0x0F];
    }
  *out = '\0';
}

#ifdef MB_TEST
#include "mb_test.h"

TEST(sha256, known_vectors) {
  mb_sha256 c;
  char h[65];
  mb_sha256_init(&c);
  mb_sha256_update(&c, "", 0);
  mb_sha256_final_hex(&c, h);
  ASSERT_STR_EQ(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  mb_sha256_init(&c);
  mb_sha256_update(&c, "abc", 3);
  mb_sha256_final_hex(&c, h);
  ASSERT_STR_EQ(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
#endif
