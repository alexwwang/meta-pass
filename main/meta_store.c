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
                             char out_hex[META_SHA256_HEX_LEN + 1])
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
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    for (int i = 0; i < 32; i++) {
        snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
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
        ESP_LOGW(TAG, "槽位 %d 镜像头预检失败,标记 INVALID", slot);
        meta_slot_mark_invalid(out);
        return;
    }
    // 权威校验:magic、segment 表、校验和、尾部 SHA-256 哈希全部由 IDF 复核。
    esp_image_metadata_t meta = {0};
    const esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) != ESP_OK) {
        ESP_LOGW(TAG, "槽位 %d esp_image_verify 失败,标记 INVALID", slot);
        meta_slot_mark_invalid(out);
        return;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        meta_slot_mark_invalid(out);
        return;
    }
    char sha_hex[META_SHA256_HEX_LEN + 1];
    if (slot_sha256(part, meta.image_len, sha_hex) != ESP_OK) {
        meta_slot_mark_invalid(out);
        return;
    }
    // 显示名 blob:读槽位尾部 sector 前 64 字节(无 PSRAM,不整 sector 读入)。
    // 偏移按分区大小动态计算(不同槽位大小不同)。解包成功则用真名替代
    // project_name;注册表 name 字段放不下(>META_NAME_LEN)时回退 project_name。
    const char *name = desc.project_name;
    char disp[META_NAME_MAX + 1];
    uint8_t blob[64];
    const uint32_t blob_off = meta_name_blob_offset(part->size);
    if (esp_partition_read(part, blob_off, blob, sizeof(blob)) == ESP_OK
            && meta_name_unpack(blob, sizeof(blob), disp, sizeof(disp))
            && strlen(disp) <= META_NAME_LEN) {
        name = disp;
    }
    if (!meta_slot_set_valid(out, name, desc.version, meta.image_len, sha_hex)) {
        meta_slot_mark_invalid(out);
        return;
    }
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
