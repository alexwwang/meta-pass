// tests/test_meta_sign.c —— 签名段格式解析 host test。
// 测 magic 检测、payload_len 校验、xor checksum、ABSENT 判定。
// 编译: cc -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain \
//         tests/test_meta_sign.c tests/esp_stubs/meta_sign_stub.c -o /tmp/test_meta_sign
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "meta_sign.h"

static int failures = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while(0)

// 构造一个有效的签名段(可变长度,测试使用 65B payload)。
static void make_valid_sig(uint8_t *buf, size_t len)
{
    memset(buf, 0, len);
    buf[0] = 'M'; buf[1] = 'S'; buf[2] = 'I'; buf[3] = 'G';
    // payload_len = 65 (LE)
    buf[4] = 65; buf[5] = 0; buf[6] = 0; buf[7] = 0;
    // 签名数据填 0xAB ([8..72])
    for (int i = 8; i < 73; i++) buf[i] = 0xAB;
    // xor checksum = 前 73 字节异或
    uint8_t xor = 0;
    for (int i = 0; i < 73; i++) xor ^= buf[i];
    buf[73] = xor;
}

// 构造一个无签名的数据区(全 0xFF = 擦除状态)。
static void make_erased(uint8_t *buf, size_t len)
{
    memset(buf, 0xFF, len);
}

// 构造一个有数据但 magic 不对的签名段。
static void make_bad_magic(uint8_t *buf, size_t len)
{
    memset(buf, 0, len);
    buf[0] = 'X'; buf[1] = 'X'; buf[2] = 'X'; buf[3] = 'X';
}

int main(void)
{
    uint8_t image[64];
    memset(image, 0x42, sizeof(image));
    uint8_t digest[32];   // host test 不真算 SHA-256, 传占位即可
    memset(digest, 0x42, sizeof(digest));

    // 1. 有效签名段 → META_SIG_OK
    {
        uint8_t sig[META_SIG_TOTAL_LEN];
        make_valid_sig(sig, sizeof(sig));
        meta_sig_result_t r = meta_sign_verify(digest, sizeof(image), sig, sizeof(sig));
        ASSERT(r == META_SIG_OK, "valid signature → META_SIG_OK");
    }

    // 2. 全 0xFF(擦除) → META_SIG_ABSENT
    {
        uint8_t sig[META_SIG_TOTAL_LEN];
        make_erased(sig, sizeof(sig));
        meta_sig_result_t r = meta_sign_verify(digest, sizeof(image), sig, sizeof(sig));
        ASSERT(r == META_SIG_ABSENT, "erased sector → META_SIG_ABSENT");
    }

    // 3. magic 不对 → META_SIG_ABSENT
    {
        uint8_t sig[META_SIG_TOTAL_LEN];
        make_bad_magic(sig, sizeof(sig));
        meta_sig_result_t r = meta_sign_verify(digest, sizeof(image), sig, sizeof(sig));
        ASSERT(r == META_SIG_ABSENT, "bad magic → META_SIG_ABSENT");
    }

    // 4. magic 对但 payload_len 不对 → META_SIG_BAD_FORMAT
    {
        uint8_t sig[META_SIG_TOTAL_LEN];
        make_valid_sig(sig, sizeof(sig));
        sig[4] = 0; sig[5] = 0; sig[6] = 0; sig[7] = 0;  // payload_len = 0
        meta_sig_result_t r = meta_sign_verify(digest, sizeof(image), sig, sizeof(sig));
        ASSERT(r == META_SIG_BAD_FORMAT, "wrong payload_len → META_SIG_BAD_FORMAT");
    }

    // 5. xor checksum 错误 → META_SIG_BAD_CHECKSUM
    {
        uint8_t sig[META_SIG_TOTAL_LEN];
        make_valid_sig(sig, sizeof(sig));
        sig[73] ^= 0x01;  // 破坏 checksum
        meta_sig_result_t r = meta_sign_verify(digest, sizeof(image), sig, sizeof(sig));
        ASSERT(r == META_SIG_BAD_CHECKSUM, "bad xor checksum → META_SIG_BAD_CHECKSUM");
    }

    // 6. 签名段太短 → META_SIG_BAD_FORMAT
    {
        uint8_t sig[16];
        memset(sig, 0, sizeof(sig));
        meta_sig_result_t r = meta_sign_verify(digest, sizeof(image), sig, sizeof(sig));
        ASSERT(r == META_SIG_BAD_FORMAT, "short buffer → META_SIG_BAD_FORMAT");
    }

    // 7. 偏移计算: image_len=100, sig_sector_offset 应为 4096
    {
        uint32_t off = meta_sign_sector_offset(100);
        ASSERT(off == 4096, "image_len=100 → sig_offset=4096");
    }

    // 8. 偏移计算: image_len=4096, sig_sector_offset 应为 4096
    {
        uint32_t off = meta_sign_sector_offset(4096);
        ASSERT(off == 4096, "image_len=4096 → sig_offset=4096");
    }

    // 9. 偏移计算: image_len=4097, sig_sector_offset 应为 8192
    {
        uint32_t off = meta_sign_sector_offset(4097);
        ASSERT(off == 8192, "image_len=4097 → sig_offset=8192");
    }

    // 10. app 上限: part_size=2MB → limit = 2MB - 4KB - 4KB
    {
        uint32_t limit = meta_sign_app_limit(0x200000);
        ASSERT(limit == 0x200000 - 0x1000 - 0x1000, "2MB part → limit = 2MB - 8KB");
    }

    // 11. 端到端: 用 sign-firmware.sh 生成的真实签名段验证
    //     (如果 /tmp/test-app-signed.bin 存在)
    {
        FILE *f = fopen("/tmp/test-app-signed.bin", "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long total = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *data = malloc(total);
            fread(data, 1, total, f);
            fclose(f);
            // image_len = 4096, sig at offset 4096
            uint32_t image_len = 4096;
            uint32_t sig_off = meta_sign_sector_offset(image_len);
            uint32_t sig_len = 8 + ((uint32_t)data[sig_off + 4]
                | ((uint32_t)data[sig_off + 5] << 8)
                | ((uint32_t)data[sig_off + 6] << 16)
                | ((uint32_t)data[sig_off + 7] << 24)) + 1;
            if (sig_off + sig_len <= (uint32_t)total) {
                meta_sig_result_t r = meta_sign_verify(data, image_len,
                                                        data + sig_off, sig_len);
                ASSERT(r == META_SIG_OK, "real signed file → META_SIG_OK");
            } else {
                printf("SKIP: real signed file too short\n");
            }
            free(data);
        } else {
            printf("SKIP: /tmp/test-app-signed.bin not found\n");
        }
    }

    printf("\n%d failures\n", failures);
    return failures ? 1 : 0;
}
