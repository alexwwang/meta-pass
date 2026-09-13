// tests/esp_stubs/meta_sign_stub.c —— host test 里替代 meta_sign.c 的桩。
// 只测签名段格式解析(magic/payload_len/xor checksum),不链接 mbedtls。
// RSA 验签留给设备侧真机验证。
#include "meta_sign.h"
#include <string.h>

static bool check_magic(const uint8_t *buf)
{
    static const unsigned char magic[4] = META_SIG_MAGIC_BYTES;
    return memcmp(buf, magic, 4) == 0;
}

static uint8_t xor_checksum(const uint8_t *buf, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= buf[i];
    return x;
}

meta_sig_result_t meta_sign_verify(const uint8_t digest[32], uint32_t image_len,
                                    const uint8_t *sig_sector, size_t sig_len)
{
    (void)digest; (void)image_len;  // host test 不做 RSA 验签
    if (sig_len < META_SIG_TOTAL_LEN) return META_SIG_BAD_FORMAT;
    if (!check_magic(sig_sector)) return META_SIG_ABSENT;

    uint32_t payload_len = (uint32_t)sig_sector[4]
                         | ((uint32_t)sig_sector[5] << 8)
                         | ((uint32_t)sig_sector[6] << 16)
                         | ((uint32_t)sig_sector[7] << 24);
    if (payload_len != META_SIG_RSA_LEN) return META_SIG_BAD_FORMAT;

    uint8_t calc = xor_checksum(sig_sector, META_SIG_TOTAL_LEN - 1);
    if (calc != sig_sector[META_SIG_TOTAL_LEN - 1]) return META_SIG_BAD_CHECKSUM;

    // host test 里不调用 mbedtls,格式检查通过即视为 OK
    return META_SIG_OK;
}
