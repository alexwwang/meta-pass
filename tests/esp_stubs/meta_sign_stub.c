// tests/esp_stubs/meta_sign_stub.c —— host test 里替代 meta_sign.c 的桩。
// 只测签名段/彩蛋段格式解析(magic/payload_len/xor checksum),不链接 mbedtls。
// ECDSA 验签留给设备侧真机验证。
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

static bool printable_ascii(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (buf[i] < 0x20 || buf[i] > 0x7E) return false;
    }
    return true;
}

meta_sig_result_t meta_sign_verify(const uint8_t digest[32], uint32_t image_len,
                                    const uint8_t *sig_sector, size_t sig_len)
{
    (void)digest; (void)image_len;  // host test 不做 ECDSA 验签
    if (sig_len < META_SIG_TOTAL_LEN) return META_SIG_BAD_FORMAT;
    if (!check_magic(sig_sector)) return META_SIG_ABSENT;

    uint32_t payload_len = (uint32_t)sig_sector[4]
                         | ((uint32_t)sig_sector[5] << 8)
                         | ((uint32_t)sig_sector[6] << 16)
                         | ((uint32_t)sig_sector[7] << 24);
    if (payload_len == 0 || payload_len > META_SIG_SIG_LEN) return META_SIG_BAD_FORMAT;

    uint32_t xor_off = META_SIG_HEADER_LEN + payload_len;
    uint8_t calc = xor_checksum(sig_sector, xor_off);
    if (calc != sig_sector[xor_off]) return META_SIG_BAD_CHECKSUM;

    // host test 里不调用 mbedtls,格式检查通过即视为 OK
    return META_SIG_OK;
}

meta_egg_result_t meta_egg_parse(const uint8_t *tail_sector, size_t tail_sector_len,
                                  char *out, size_t out_cap)
{
    if (!tail_sector || !out || out_cap < 2) return META_EGG_BAD_FORMAT;
    if (tail_sector_len < META_NAME_BLOB_OFF) return META_EGG_BAD_FORMAT;
    if ((uint32_t)(META_EGG_WINDOW_OFF + META_EGG_TOTAL_LEN) > META_NAME_BLOB_OFF) return META_EGG_BAD_FORMAT;

    const uint8_t *egg = tail_sector + META_EGG_WINDOW_OFF;
    static const uint8_t egg_magic[4] = META_EGG_MAGIC_BYTES;
    if (memcmp(egg, egg_magic, 4) != 0) return META_EGG_ABSENT;

    uint32_t payload_len = (uint32_t)egg[4]
                         | ((uint32_t)egg[5] << 8)
                         | ((uint32_t)egg[6] << 16)
                         | ((uint32_t)egg[7] << 24);
    if (payload_len == 0 || payload_len > META_EGG_TEXT_LEN) return META_EGG_BAD_FORMAT;

    // 定长窗口: xor 固定在窗口末字节,覆盖 magic+len+整个 text 区(含 0xFF padding)。
    uint8_t calc = xor_checksum(egg, META_EGG_XOR_OFF);
    if (calc != egg[META_EGG_XOR_OFF]) return META_EGG_BAD_CHECKSUM;

    if (!printable_ascii(egg + META_EGG_HEADER_LEN, payload_len)) return META_EGG_BAD_FORMAT;
    if (out_cap < (size_t)payload_len + 1) return META_EGG_BAD_FORMAT;

    memcpy(out, egg + META_EGG_HEADER_LEN, payload_len);
    out[payload_len] = '\0';
    return META_EGG_OK;
}
