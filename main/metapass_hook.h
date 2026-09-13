// main/metapass_hook.h —— 子固件适配 meta-pass 启动器的最小 hook(header-only)。
//
// 用法(子固件):
//   1. 启动自检通过后调用 metapass_mark_valid()  → 跨重启常驻,否则下次重启自动回启动器。
//      ★ 内部先验签:签名徽章存在且验签通过才允许常驻;未签名返回 ESP_ERR_NOT_SUPPORTED,
//        OTA 保持"待验证"态 → 下次重启自动回退启动器(trial boot)。
//   2. 在按键处理里响应 OK 键的 BSP_BTN_LONG2 → 调 metapass_return_to_launcher() 退回启动器。
//      (注意:LONG2 触发前会先触发一次 LONG,应用内"返回"与"退回启动器"会先后发生;
//       设计应用交互时把 LONG 安排为可安全先发生的动作,如返回上一页。)
//
// 只有返回启动器和常驻两个能力需要适配;不适配的子固件表现为"试运行"(重启即回启动器)。
//
// CMakeLists 依赖:REQUIRES mbedtls
// 签名徽章格式见 docs/assets/meta-pass-design.md §7。
// 签名工具:tools/signing/sign-firmware.sh <app.bin>
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_image_format.h"   // esp_image_verify / esp_image_metadata_t
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509.h"

// ── 签名徽章格式(与 main/meta_sign.h 一致) ──────────────────────────
#define MP_SIG_MAGIC     "MSIG"
#define MP_SIG_HDR_LEN   8u   // magic(4) + payload_len(4)
#define MP_SIG_RSA_LEN   256u // RSA-2048 签名
#define MP_SIG_TOTAL     265u // 8 + 256 + 1(xor)
#define MP_SIG_SECTOR    4096u
#define MP_BLOB_SECTOR   4096u // name blob sector

// meta-pass 内置公钥(RSA-2048, SubjectPublicKeyInfo DER)。
// 生成:tools/signing/gen-pubkey.py(从 private.pem 读取真实 DER,勿手抄)
// 更新此数组后所有子固件需重新编译才能验证新签名。
// MP_PUBKEY_BEGIN — 下方数组由 tools/signing/gen-pubkey.py 生成,勿手改
#define MP_PUBKEY_LEN 294
static const unsigned char mp_pubkey_der[] = {
  0x30, 0x82, 0x01, 0x22, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
  0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00, 0x03, 0x82, 0x01, 0x0f, 0x00,
  0x30, 0x82, 0x01, 0x0a, 0x02, 0x82, 0x01, 0x01, 0x00, 0xbb, 0x5e, 0xc2,
  0x3f, 0x91, 0x98, 0x2f, 0x0e, 0x23, 0xbe, 0x6d, 0x02, 0xb1, 0xe3, 0xc2,
  0x5c, 0x76, 0x86, 0x02, 0xed, 0x8c, 0xe1, 0x98, 0xe1, 0xa0, 0xae, 0x1d,
  0x28, 0x68, 0x02, 0x4c, 0x5a, 0x51, 0x30, 0x50, 0x12, 0x3e, 0xe4, 0x12,
  0x69, 0x0e, 0xe7, 0x64, 0x8a, 0x20, 0xff, 0x6f, 0x7c, 0xf9, 0xe0, 0x21,
  0x68, 0xd3, 0xb3, 0x85, 0xcb, 0x03, 0x75, 0xdf, 0xf4, 0x8d, 0xce, 0xed,
  0x1e, 0x92, 0x7b, 0x6a, 0x3d, 0xe7, 0x9d, 0xe3, 0xb2, 0x81, 0x23, 0xa2,
  0xa3, 0xa7, 0xed, 0xea, 0x6e, 0x09, 0xaa, 0xc7, 0x77, 0xee, 0x4d, 0x91,
  0x2d, 0xbc, 0x42, 0x3f, 0x7e, 0x57, 0xbc, 0xa6, 0x03, 0xcb, 0x0e, 0xa1,
  0x84, 0x35, 0x66, 0x04, 0xa8, 0xef, 0x16, 0x74, 0x2a, 0x33, 0x52, 0xcd,
  0xc7, 0x34, 0x20, 0xec, 0x10, 0xd0, 0xf6, 0x0d, 0xef, 0x8d, 0x72, 0x84,
  0x99, 0xb8, 0xc1, 0x00, 0x24, 0xc9, 0x8f, 0x5f, 0xc4, 0x67, 0xad, 0x3d,
  0xa3, 0x01, 0xec, 0x4d, 0xd5, 0x8d, 0x38, 0x20, 0x89, 0xa1, 0x23, 0x29,
  0xbd, 0xf7, 0xc5, 0x4f, 0x41, 0xff, 0xf2, 0xbf, 0x64, 0xd5, 0xba, 0xd2,
  0x09, 0x0c, 0xf0, 0xb1, 0x35, 0x56, 0x99, 0x70, 0x81, 0x82, 0x41, 0x75,
  0x74, 0xc9, 0x09, 0x1f, 0xfe, 0x7e, 0x0d, 0x73, 0xae, 0xcb, 0x84, 0x7c,
  0x2d, 0x20, 0x35, 0xc2, 0x7d, 0x3f, 0xbe, 0xda, 0x02, 0x96, 0x82, 0x56,
  0xa5, 0xea, 0x7c, 0xaa, 0xc9, 0x6c, 0x62, 0x04, 0xe0, 0x1b, 0x75, 0x7d,
  0xa3, 0xe4, 0x50, 0x6f, 0xb9, 0x66, 0x92, 0x3c, 0x93, 0xbe, 0x76, 0xfb,
  0xed, 0x13, 0x5c, 0xab, 0x0f, 0x9b, 0x28, 0x8c, 0xd7, 0x4a, 0x8e, 0x7f,
  0x77, 0xfb, 0xc5, 0x64, 0xbe, 0x1f, 0x3e, 0x7b, 0xba, 0x4b, 0xf1, 0x51,
  0x93, 0xe0, 0x75, 0x4b, 0x1b, 0x1e, 0x61, 0xde, 0x48, 0x23, 0xed, 0x89,
  0xef, 0x02, 0x03, 0x01, 0x00, 0x01,
};
// MP_PUBKEY_END

// ── 内部:流式计算分区前 n 字节 SHA-256(4KB 分块,无整包入 RAM) ──────
static int mp_stream_sha256(const esp_partition_t *part, uint32_t len, uint8_t out[32])
{
    uint8_t *buf = malloc(4096);
    if (!buf) return -1;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    for (uint32_t off = 0; off < len;) {
        uint32_t chunk = (len - off < 4096) ? (len - off) : 4096;
        if (esp_partition_read(part, off, buf, chunk) != ESP_OK) {
            free(buf); mbedtls_sha256_free(&sha); return -1;
        }
        mbedtls_sha256_update(&sha, buf, chunk);
        off += chunk;
    }
    free(buf);
    mbedtls_sha256_finish(&sha, out);
    mbedtls_sha256_free(&sha);
    return 0;
}

// ── 验签:检查当前运行固件是否携带有效签名徽章 ──────────────────────
// 布局:[app image (image_len)] [sig sector (4KB)] [name blob sector (4KB)]
// esp_image_verify 只校验 image_len 范围;签名 sector 在其后,安全。
// 返回 true = 签名有效;false = 无签名或验签失败。
static bool mp_is_current_app_signed(void)
{
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (!part) return false;

    // 1. 取 image_len
    esp_image_metadata_t meta = {0};
    esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) return false;

    // 2. 签名 sector 偏移:image_len 之后,4K 对齐
    uint32_t sig_off = (meta.image_len + MP_SIG_SECTOR - 1u) & ~(MP_SIG_SECTOR - 1u);
    uint32_t blob_off = part->size - MP_BLOB_SECTOR;
    if (sig_off + MP_SIG_TOTAL > blob_off) return false; // 空间不够

    // 3. 读签名 sector 前 265 字节
    uint8_t sig[MP_SIG_TOTAL];
    if (esp_partition_read(part, sig_off, sig, sizeof(sig)) != ESP_OK) return false;

    // 4. 检查 magic
    if (memcmp(sig, MP_SIG_MAGIC, 4) != 0) return false;

    // 5. payload_len
    uint32_t payload_len = (uint32_t)sig[4]
                         | ((uint32_t)sig[5] << 8)
                         | ((uint32_t)sig[6] << 16)
                         | ((uint32_t)sig[7] << 24);
    if (payload_len != MP_SIG_RSA_LEN) return false;

    // 6. xor checksum(前 264 字节异或应等于第 265 字节)
    uint8_t xor = 0;
    for (uint32_t i = 0; i < MP_SIG_TOTAL - 1; i++) xor ^= sig[i];
    if (xor != sig[MP_SIG_TOTAL - 1]) return false;

    // 7. 计算 image 的 SHA-256
    uint8_t digest[32];
    if (mp_stream_sha256(part, meta.image_len, digest) != 0) return false;

    // 8. RSA-PKCS1v1.5 验签
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    if (mbedtls_pk_parse_public_key(&pk, mp_pubkey_der, MP_PUBKEY_LEN) != 0) {
        mbedtls_pk_free(&pk); return false;
    }
    int rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                               digest, 32,
                               sig + MP_SIG_HDR_LEN, MP_SIG_RSA_LEN);
    mbedtls_pk_free(&pk);
    return rc == 0;
}

// ── 自检通过后调用:标记当前固件有效,取消自动回滚 ────────────────────
// 签名固件 → 允许常驻;未签名 → 返回错误,OTA 保持待验证态 → 下次重启自动回启动器。
// 返回 ESP_OK 表示已常驻;ESP_ERR_NOT_SUPPORTED 表示未签名(试运行);
// 其他值表示非 OTA 启动(如直接从 factory 调试运行),可忽略。
static inline esp_err_t metapass_mark_valid(void)
{
    if (!mp_is_current_app_signed()) {
        return ESP_ERR_NOT_SUPPORTED;  // 未签名 → 不常驻,下次重启回启动器
    }
    return esp_ota_mark_app_valid_cancel_rollback();
}

// 把启动分区切回 factory(meta-pass 启动器)并立即重启,不返回。
static inline void metapass_return_to_launcher(void)
{
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) {
        esp_ota_set_boot_partition(factory);
    }
    esp_restart();
}
