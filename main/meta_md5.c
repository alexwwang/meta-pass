// main/meta_md5.c —— RFC 1321 MD5(实现即规范,黄金向量钉死)。
#include "meta_md5.h"

#include <string.h>

#if defined(META_MD5_USE_ROM)

/* The second-stage bootloader already ships the ESP32-C3 ROM MD5 engine.
 * Reuse it here instead of carrying the 64-word RFC-1321 K table + rotation
 * table in bootloader DRAM rodata (~320 bytes).  The host/app implementation
 * below remains self-contained so the same public API and golden vectors stay
 * available outside the bootloader. */
#include "esp_rom_md5.h"

void meta_md5(const void *data, size_t len, uint8_t out[16])
{
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, (const unsigned char *)data, len);
    esp_rom_md5_final(out, &ctx);
}

#else

typedef struct {
    uint32_t state[4];
    uint64_t nbytes;
    uint8_t  block[64];
} md5_ctx_t;

static uint32_t rotl32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static const uint32_t K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

// 每轮的循环移位量(规范表)。
static const uint8_t S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void md5_block(md5_ctx_t *c, const uint8_t *p)
{
    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = load_le32(p + i * 4);
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & cc) | (~b & d);
            g = (uint32_t)i;
        } else if (i < 32) {
            f = (d & b) | (~d & cc);
            g = (5u * (uint32_t)i + 1u) % 16u;
        } else if (i < 48) {
            f = b ^ cc ^ d;
            g = (3u * (uint32_t)i + 5u) % 16u;
        } else {
            f = cc ^ (b | ~d);
            g = (7u * (uint32_t)i) % 16u;
        }
        uint32_t tmp = d;
        d = cc;
        cc = b;
        b = b + rotl32(a + f + K[i] + M[g], S[i]);
        a = tmp;
    }
    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
}

static void md5_init(md5_ctx_t *c)
{
    c->state[0] = 0x67452301u;
    c->state[1] = 0xefcdab89u;
    c->state[2] = 0x98badcfeu;
    c->state[3] = 0x10325476u;
    c->nbytes = 0;
}

static void md5_update(md5_ctx_t *c, const uint8_t *data, size_t len)
{
    size_t used = (size_t)(c->nbytes & 63u);
    c->nbytes += len;
    if (used != 0) {
        size_t fill = 64u - used;
        if (len < fill) {
            memcpy(c->block + used, data, len);
            return;
        }
        memcpy(c->block + used, data, fill);
        md5_block(c, c->block);
        data += fill;
        len -= fill;
    }
    while (len >= 64u) {
        md5_block(c, data);
        data += 64u;
        len -= 64u;
    }
    if (len != 0) {
        memcpy(c->block, data, len);
    }
}

static void md5_final(md5_ctx_t *c, uint8_t out[16])
{
    uint64_t bits = c->nbytes * 8u;
    static const uint8_t pad1 = 0x80;
    static const uint8_t zeros[56] = {0};
    size_t used = (size_t)(c->nbytes & 63u);
    size_t pad_len = (used < 56u) ? (56u - used) : (120u - used);
    md5_update(c, &pad1, 1);
    md5_update(c, zeros, pad_len - 1);
    uint8_t len_le[8];
    for (int i = 0; i < 8; i++) {
        len_le[i] = (uint8_t)(bits >> (8 * i));
    }
    md5_update(c, len_le, 8);
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            out[i * 4 + k] = (uint8_t)(c->state[i] >> (8 * k));
        }
    }
}

void meta_md5(const void *data, size_t len, uint8_t out[16])
{
    md5_ctx_t c;
    md5_init(&c);
    md5_update(&c, (const uint8_t *)data, len);
    md5_final(&c, out);
}


#endif
