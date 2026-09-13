#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Real SHA-256 (FIPS 180-4) for host tests — no ESP-IDF dependency. */

typedef struct mbedtls_sha256_context {
    uint8_t buffer[64];
    uint32_t total[2];
    uint32_t state[8];
    int is224;
} mbedtls_sha256_context;

/* Round constants */
static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA256_SHR(x, n)  ((x) >> (n))
#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(uint32_t state[8], const uint8_t data[64])
{
    uint32_t m[64], a, b, c, d, e, f, g, h;
    for (int i = 0, j = 0; i < 16; i++, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) | ((uint32_t)data[j+3]);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = SHA256_ROTR(m[i-15], 7) ^ SHA256_ROTR(m[i-15], 18) ^ SHA256_SHR(m[i-15], 3);
        uint32_t s1 = SHA256_ROTR(m[i-2], 17) ^ SHA256_ROTR(m[i-2], 19) ^ SHA256_SHR(m[i-2], 10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = SHA256_ROTR(e, 6) ^ SHA256_ROTR(e, 11) ^ SHA256_ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + m[i];
        uint32_t S0 = SHA256_ROTR(a, 2) ^ SHA256_ROTR(a, 13) ^ SHA256_ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static inline void mbedtls_sha256_init(mbedtls_sha256_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

static inline void mbedtls_sha256_free(mbedtls_sha256_context *ctx)
{
    if (ctx) memset(ctx, 0, sizeof(*ctx));
}

static inline int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224)
{
    ctx->total[0] = 0;
    ctx->total[1] = 0;
    ctx->is224 = is224;
    if (is224) {
        ctx->state[0] = 0xc1059ed8; ctx->state[1] = 0x367cd507;
        ctx->state[2] = 0x3070dd17; ctx->state[3] = 0xf70e5939;
        ctx->state[4] = 0xffc00b31; ctx->state[5] = 0x68581511;
        ctx->state[6] = 0x64f98fa7; ctx->state[7] = 0xbefa4fa4;
    } else {
        ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
        ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
        ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
        ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    }
    return 0;
}

static inline int mbedtls_sha256_update(mbedtls_sha256_context *ctx,
                                         const unsigned char *input, size_t ilen)
{
    size_t fill;
    uint32_t left;
    if (ilen == 0) return 0;
    left = ctx->total[0] & 0x3F;
    fill = 64 - left;
    ctx->total[0] += (uint32_t)ilen;
    ctx->total[0] &= 0xFFFFFFFF;
    if (ctx->total[0] < (uint32_t)ilen) ctx->total[1]++;
    if (left && ilen >= fill) {
        memcpy(ctx->buffer + left, input, fill);
        sha256_transform(ctx->state, ctx->buffer);
        input += fill;
        ilen  -= fill;
        left   = 0;
    }
    while (ilen >= 64) {
        sha256_transform(ctx->state, input);
        input += 64;
        ilen  -= 64;
    }
    if (ilen > 0) memcpy(ctx->buffer + left, input, ilen);
    return 0;
}

static inline int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char *output)
{
    uint32_t last, padn;
    uint32_t high, low;
    unsigned char msglen[8];
    high = (ctx->total[0] >> 29) | (ctx->total[1] << 3);
    low  = ctx->total[0] << 3;
    msglen[0] = (unsigned char)(high >> 24);
    msglen[1] = (unsigned char)(high >> 16);
    msglen[2] = (unsigned char)(high >> 8);
    msglen[3] = (unsigned char)high;
    msglen[4] = (unsigned char)(low >> 24);
    msglen[5] = (unsigned char)(low >> 16);
    msglen[6] = (unsigned char)(low >> 8);
    msglen[7] = (unsigned char)low;
    last = ctx->total[0] & 0x3F;
    padn = (last < 56) ? (56 - last) : (120 - last);
    /* padding + length */
    {
        unsigned char pad[128];
        memset(pad, 0, sizeof(pad));
        pad[0] = 0x80;
        mbedtls_sha256_update(ctx, pad, padn);
        mbedtls_sha256_update(ctx, msglen, 8);
    }
    for (int i = 0; i < 8; i++) {
        output[i*4]   = (unsigned char)(ctx->state[i] >> 24);
        output[i*4+1] = (unsigned char)(ctx->state[i] >> 16);
        output[i*4+2] = (unsigned char)(ctx->state[i] >> 8);
        output[i*4+3] = (unsigned char)(ctx->state[i]);
    }
    return 0;
}
