// main/meta_store.c —— 实现见头文件注释。
#include "meta_store.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"   // esp_image_verify / esp_image_metadata_t
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

#include "meta_image.h"
#include "meta_name.h"
#include "meta_sign.h"
static const char *TAG = "meta_store";

// 槽位与分区 subtype 的固定映射:ota_0/ota_1/ota_2(见 partitions.csv)。
const esp_partition_t *meta_store_slot_partition(int slot)
{
    if (slot < 0 || slot >= META_SLOT_COUNT) return NULL;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot),
                                    NULL);
}

// 流式计算分区前 len 字节的 SHA-256(4KB 分块读,不整包入 RAM;无 PSRAM 约束)。
static esp_err_t slot_sha256(const esp_partition_t *part, uint32_t len,
                             uint8_t out_digest[32], char out_hex[META_SHA256_HEX_LEN + 1])
{
    uint8_t *buf = malloc(4096);
    if (!buf) return ESP_ERR_NO_MEM;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);   // 0 = SHA-256(非 SHA-224)
    esp_err_t err = ESP_OK;
    for (uint32_t off = 0; off < len;) {
        const uint32_t chunk = (len - off < 4096) ? (len - off) : 4096;
        err = esp_partition_read(part, off, buf, chunk);
        if (err != ESP_OK) break;
        mbedtls_sha256_update(&sha, buf, chunk);
        off += chunk;
    }
    free(buf);
    if (err != ESP_OK) {
        mbedtls_sha256_free(&sha);
        return err;
    }
    mbedtls_sha256_finish(&sha, out_digest);
    mbedtls_sha256_free(&sha);
    for (int i = 0; i < 32; i++) {
        snprintf(out_hex + i * 2, 3, "%02x", out_digest[i]);
    }
    out_hex[META_SHA256_HEX_LEN] = '\0';
    return ESP_OK;
}

// 单槽扫描:快速预检 → esp_image_verify 权威校验 → 元数据 + SHA-256。
static void scan_one(int slot, meta_slot_info_t *out)
{
    meta_slot_clear(out);
    const esp_partition_t *part = meta_store_slot_partition(slot);
    if (!part) {
        ESP_LOGE(TAG, "槽位 %d 分区不存在(分区表被破坏?)", slot);
        return;
    }
    uint8_t hdr[META_IMAGE_HEADER_LEN];
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK) {
        ESP_LOGE(TAG, "槽位 %d 读取失败", slot);
        return;
    }
    // 全 0xFF = 已擦除的空槽。
    bool erased = true;
    for (size_t i = 0; i < sizeof(hdr); i++) {
        if (hdr[i] != 0xFF) { erased = false; break; }
    }
    if (erased) return;   // META_SLOT_EMPTY

    if (meta_image_check_header(hdr, sizeof(hdr)) != META_IMG_OK) {
        meta_slot_mark_invalid(out);
        return;
    }
    esp_image_metadata_t meta = {0};
    const esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) != ESP_OK) {
        meta_slot_mark_invalid(out);
        return;
    }
    const uint32_t image_len = meta.image_len;
    uint32_t app_max = meta_name_max_app_size(part->size);
    if (image_len > app_max) {
        meta_slot_mark_invalid(out);
        return;
    }

    // 流式计算 SHA-256(避免把整个镜像载入 SRAM)。
    uint8_t sha_digest[32];
    char sha_hex[META_SHA256_HEX_LEN + 1];
    if (slot_sha256(part, image_len, sha_digest, sha_hex) != ESP_OK) {
        meta_slot_mark_invalid(out);
        return;
    }

    // metadata sector:紧跟 image_len 后 4K 对齐;MSIG/MAEG/MNAM 同 sector。
    // 注意:static 缓冲区避免 4KB 上栈;scan_one 只在启动扫描路径串行调用。
    const uint32_t tail_off = meta_sign_sector_offset(image_len);
    out->name[0] = '\0';
    if (tail_off + META_SIG_SECTOR <= part->size) {
        static uint8_t tail_sector[META_SIG_SECTOR];
        if (esp_partition_read(part, tail_off, tail_sector, sizeof(tail_sector)) == ESP_OK) {
            const meta_sig_result_t sr = meta_sign_verify(sha_digest, image_len,
                                                          tail_sector, sizeof(tail_sector));
            out->signed_fw = (sr == META_SIG_OK);
            meta_name_unpack_tail(tail_sector + META_NAME_BLOB_OFF, META_NAME_BLOB_LEN,
                                  out->name, sizeof(out->name));
            ESP_LOGI(TAG, "槽位 %d 签名: %s (%d)", slot,
                     out->signed_fw ? "SIGNED" : "unsigned", sr);
        }
    }

    // 填充有效槽信息。
    // 注意: esp_app_get_description() 返回的是正在运行的 meta-pass 自身 desc,
    // 不是槽位子固件的——版本字段仅为占位(沿用既有行为)。
    const esp_app_desc_t *desc = esp_app_get_description();
    meta_slot_set_valid(out, out->name, desc ? desc->version : "", image_len, sha_hex);
    ESP_LOGI(TAG, "槽位 %d: %s %s (%lu B)", slot, out->name, out->version,
             (unsigned long)out->size);
}

esp_err_t meta_store_scan(meta_slot_info_t out[META_SLOT_COUNT])
{
    if (!out) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < META_SLOT_COUNT; i++) {
        scan_one(i, &out[i]);
    }
    return ESP_OK;
}

esp_err_t meta_store_erase_slot(int slot)
{
    const esp_partition_t *part = meta_store_slot_partition(slot);
    if (!part) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "擦除槽位 %d(%s @0x%lx, %lu B)", slot, part->label,
             (unsigned long)part->address, (unsigned long)part->size);
    return esp_partition_erase_range(part, 0, part->size);
}

esp_err_t meta_store_boot_slot(int slot)
{
    const esp_partition_t *part = meta_store_slot_partition(slot);
    if (!part) return ESP_ERR_INVALID_ARG;
    return esp_ota_set_boot_partition(part);
}

esp_err_t meta_store_mark_factory_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}
