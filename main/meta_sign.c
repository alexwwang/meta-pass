// main/meta_sign.c —— 应用层签名徽章验签实现。
// 依赖 mbedtls 做 RSA-PKCS1v1.5 验签;SHA-256 digest 由调用方流式计算后传入,
// 避免整包镜像入 RAM(ESP32-C3 无 PSRAM, SRAM ~400KB, 镜像可达 1.5MB)。
#include "meta_sign.h"
#include "meta_sign_pubkey.h"

#include <string.h>
#include "mbedtls/rsa.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509.h"

// 检查 magic "MSIG"。
static bool check_magic(const uint8_t *buf)
{
    static const unsigned char magic[4] = META_SIG_MAGIC_BYTES;
    return memcmp(buf, magic, 4) == 0;
}

// 计算前 n 字节的 XOR checksum(逐字节异或)。
static uint8_t xor_checksum(const uint8_t *buf, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= buf[i];
    return x;
}

meta_sig_result_t meta_sign_verify(const uint8_t digest[32], uint32_t image_len,
                                    const uint8_t *sig_sector, size_t sig_len)
{
    if (sig_len < META_SIG_TOTAL_LEN) return META_SIG_BAD_FORMAT;
    if (!check_magic(sig_sector)) return META_SIG_ABSENT;  // 不是 MSIG → 无签名

    // payload_len 应为 RSA-2048 签名长度
    uint32_t payload_len = (uint32_t)sig_sector[4]
                         | ((uint32_t)sig_sector[5] << 8)
                         | ((uint32_t)sig_sector[6] << 16)
                         | ((uint32_t)sig_sector[7] << 24);
    if (payload_len != META_SIG_RSA_LEN) return META_SIG_BAD_FORMAT;

    // xor checksum: 前 264 字节(magic+payload_len+signature)的异或应等于第 265 字节
    uint8_t calc = xor_checksum(sig_sector, META_SIG_TOTAL_LEN - 1);
    if (calc != sig_sector[META_SIG_TOTAL_LEN - 1]) return META_SIG_BAD_CHECKSUM;

    // 解析公钥并用 RSA-PKCS1-v1.5 验签
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_parse_public_key(&pk, metapass_sign_pubkey_der,
                                          METAPASS_SIGN_PUBKEY_LEN);
    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return META_SIG_VERIFY_FAIL;
    }

    // 验签:对 SHA-256 digest 做 RSA-PKCS1-v1.5 验签
    ret = mbedtls_pk_verify(&pk,
                             MBEDTLS_MD_SHA256,
                             digest, 32,
                             sig_sector + META_SIG_HEADER_LEN,
                             META_SIG_RSA_LEN);
    mbedtls_pk_free(&pk);
    (void)image_len;  // 仅用于日志/调试,不参与验签
    return (ret == 0) ? META_SIG_OK : META_SIG_VERIFY_FAIL;
}
