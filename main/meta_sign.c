// main/meta_sign.c —— 应用层签名徽章验签实现。
// 依赖 mbedtls 做 ECDSA-P256 验签;SHA-256 digest 由调用方流式计算后传入,
// 避免整包镜像入 RAM(ESP32-C3 无 PSRAM, SRAM ~400KB, 镜像可达 1.5MB)。
#ifdef HOST_TEST
// 主机测试模式:使用 stub 实现,不进行真实 ECDSA 验签
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
    (void)digest; (void)image_len;
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
    return META_SIG_OK;
}
bool meta_sign_detect_sector(const uint8_t *sig_sector, size_t sig_len)
{
    return meta_sign_verify(NULL, 0, sig_sector, sig_len) == META_SIG_OK;
}
meta_egg_result_t meta_egg_parse(const uint8_t *tail_sector, size_t tail_sector_len,
                                  char *out, size_t out_cap)
{
    if (!tail_sector || !out || out_cap < 2) return META_EGG_BAD_FORMAT;
    if (tail_sector_len < META_NAME_BLOB_OFF) return META_EGG_BAD_FORMAT;
    if ((uint32_t)(META_EGG_WINDOW_OFF + META_EGG_TOTAL_LEN) > META_NAME_BLOB_OFF) return META_EGG_BAD_FORMAT;
    const uint8_t *egg = tail_sector + META_EGG_WINDOW_OFF;
    static const unsigned char egg_magic[4] = META_EGG_MAGIC_BYTES;
    if (!memcmp(egg, egg_magic, 4)) return META_EGG_ABSENT;
    uint32_t payload_len = (uint32_t)egg[4] | ((uint32_t)egg[5] << 8) | ((uint32_t)egg[6] << 16) | ((uint32_t)egg[7] << 24);
    if (payload_len == 0 || payload_len > META_EGG_TEXT_LEN) return META_EGG_BAD_FORMAT;
    uint8_t calc = 0;
    for (size_t i = 0; i < META_EGG_XOR_OFF; i++) calc ^= egg[i];
    if (calc != egg[META_EGG_XOR_OFF]) return META_EGG_BAD_CHECKSUM;
    for (uint32_t i = 0; i < payload_len; i++) {
        if (egg[META_EGG_HEADER_LEN + i] < 0x20 || egg[META_EGG_HEADER_LEN + i] > 0x7E) return META_EGG_BAD_FORMAT;
    }
    if (out_cap < (size_t)payload_len + 1) return META_EGG_BAD_FORMAT;
    memcpy(out, egg + META_EGG_HEADER_LEN, payload_len);
    out[payload_len] = '\0';
    return META_EGG_OK;
}
#else
#include "meta_sign.h"
#include "meta_sign_pubkey.h"
#include <string.h>
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
// 计算前 n 字节的 XOR checksum(逐字节异或)。
static uint8_t xor_checksum(const uint8_t *buf, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= buf[i];
    return x;
}

static bool check_egg_magic(const uint8_t *buf)
{
    static const unsigned char magic[4] = META_EGG_MAGIC_BYTES;
    return memcmp(buf, magic, 4) == 0;
}

static bool printable_ascii(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (text[i] < 0x20 || text[i] > 0x7E) return false;
    }
    return true;
}

meta_egg_result_t meta_egg_parse(const uint8_t *tail_sector, size_t tail_sector_len,
                                  char *out, size_t out_cap)
{
    if (!tail_sector || !out || out_cap < 2) return META_EGG_BAD_FORMAT;
    if (tail_sector_len < META_NAME_BLOB_OFF) return META_EGG_BAD_FORMAT;
    if ((uint32_t)(META_EGG_WINDOW_OFF + META_EGG_TOTAL_LEN) > META_NAME_BLOB_OFF) return META_EGG_BAD_FORMAT;

    const uint8_t *egg = tail_sector + META_EGG_WINDOW_OFF;
    if (!check_egg_magic(egg)) return META_EGG_ABSENT;

    uint32_t payload_len = (uint32_t)egg[4]
                         | ((uint32_t)egg[5] << 8)
                         | ((uint32_t)egg[6] << 16)
                         | ((uint32_t)egg[7] << 24);
    if (payload_len == 0 || payload_len > META_EGG_TEXT_LEN) return META_EGG_BAD_FORMAT;

    // 定长窗口: xor 固定在窗口末字节,覆盖 magic+len+整个 text 区(含 0xFF padding)。
    uint8_t calc = xor_checksum(egg, META_EGG_XOR_OFF);
    if (calc != egg[META_EGG_XOR_OFF]) return META_EGG_BAD_CHECKSUM;

    if (!printable_ascii((const char *)(egg + META_EGG_HEADER_LEN), payload_len)) return META_EGG_BAD_FORMAT;
    if (out_cap < (size_t)payload_len + 1) return META_EGG_BAD_FORMAT;

    memcpy(out, egg + META_EGG_HEADER_LEN, payload_len);
    out[payload_len] = '\0';
    return META_EGG_OK;
}

static bool check_magic(const uint8_t *buf)
{
    static const unsigned char magic[4] = META_SIG_MAGIC_BYTES;
    return memcmp(buf, magic, 4) == 0;
}


meta_sig_result_t meta_sign_verify(const uint8_t digest[32], uint32_t image_len,
                                    const uint8_t *sig_sector, size_t sig_len)
{
    if (sig_len < META_SIG_TOTAL_LEN) return META_SIG_BAD_FORMAT;
    if (!check_magic(sig_sector)) return META_SIG_ABSENT;  // 不是 MSIG → 无签名

    // payload_len 应为 ECDSA-P256 DER 签名长度(64..72 字节)
    uint32_t payload_len = (uint32_t)sig_sector[4]
                         | ((uint32_t)sig_sector[5] << 8)
                         | ((uint32_t)sig_sector[6] << 16)
                         | ((uint32_t)sig_sector[7] << 24);
    if (payload_len == 0 || payload_len > META_SIG_SIG_LEN) return META_SIG_BAD_FORMAT;
    // xor checksum: magic+payload_len+signature 的异或应等于紧随其后的 1 字节
    uint32_t xor_off = META_SIG_HEADER_LEN + payload_len;
    uint8_t calc = xor_checksum(sig_sector, xor_off);
    if (calc != sig_sector[xor_off]) return META_SIG_BAD_CHECKSUM;

    // 解析公钥并用 ECDSA-P256 验签
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_parse_public_key(&pk, metapass_sign_pubkey_der,
                                          METAPASS_SIGN_PUBKEY_LEN);
    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return META_SIG_VERIFY_FAIL;
    }

    // 验签:对 SHA-256 digest 做 ECDSA-P256 验签(DER 编码,payload_len 字节)
    ret = mbedtls_pk_verify(&pk,
                             MBEDTLS_MD_SHA256,
                             digest, 32,
                             sig_sector + META_SIG_HEADER_LEN,
                             payload_len);
    mbedtls_pk_free(&pk);
    (void)image_len;  // 仅用于日志/调试,不参与验签
    return (ret == 0) ? META_SIG_OK : META_SIG_VERIFY_FAIL;
}

// 轻量格式探测:仅校验 MSIG magic + xor checksum，不验 pubkey。
// 返回 true 表示尾部分区已包含合法签名结构。
bool meta_sign_detect_sector(const uint8_t *sig_sector, size_t sig_len)
{
    if (sig_len < META_SIG_TOTAL_LEN) return false;
    if (!check_magic(sig_sector)) return false;

    uint32_t payload_len = (uint32_t)sig_sector[4]
                         | ((uint32_t)sig_sector[5] << 8)
                         | ((uint32_t)sig_sector[6] << 16)
                         | ((uint32_t)sig_sector[7] << 24);
    if (payload_len == 0 || payload_len > META_SIG_SIG_LEN) return false;

    uint32_t xor_off = META_SIG_HEADER_LEN + payload_len;
    uint8_t calc = xor_checksum(sig_sector, xor_off);
    return calc == sig_sector[xor_off];
}

#endif
