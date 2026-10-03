// main/meta_backup.c —— M5 备份文件格式实现
#include "meta_backup.h"
#include "meta_carve.h"
#include "esp_err.h"
#include <string.h>
#include <stdio.h>

bool meta_backup_header_valid(const meta_backup_header_t *h)
{
    if (h->magic != META_BACKUP_MAGIC) return false;
    if (h->version != META_BACKUP_VERSION) return false;
    if (h->play_id == 0) return false;
    if (h->data_count > META_BACKUP_DATA_MAX) return false;
    // firmware_version 必须是非空可打印字符串
    if (strlen(h->firmware_version) == 0) return false;
    return true;
}

bool meta_backup_firmware_match(const meta_backup_header_t *h, const char *current_version)
{
    if (!meta_backup_header_valid(h)) return false;
    if (current_version == NULL || strlen(current_version) == 0) return false;
    return strcmp(h->firmware_version, current_version) == 0;
}

int meta_backup_serialize(const meta_carve_t *carve, uint32_t play_id,
                           meta_backup_data_t out[META_BACKUP_DATA_MAX])
{
    if (carve == NULL || out == NULL) return 0;
    if (play_id == 0) return 0;

    int count = 0;
    for (uint8_t i = 0; i < carve->data_count && count < (int)META_BACKUP_DATA_MAX; i++) {
        if (carve->data[i].play_id != play_id) continue;
        if (carve->data[i].state != META_DATA_ARCHIVED) continue;
        if (carve->data[i].size == 0) continue;

        out[count].play_id = carve->data[i].play_id;
        out[count].offset = carve->data[i].offset;
        out[count].size = carve->data[i].size;
        out[count].state = carve->data[i].state;
        out[count].type = carve->data[i].type;
        out[count].subtype = carve->data[i].subtype;
        strncpy(out[count].label, carve->data[i].label, sizeof(out[count].label) - 1);
        out[count].label[sizeof(out[count].label) - 1] = '\0';
        count++;
    }
    return count;
}

esp_err_t meta_backup_deserialize(const meta_backup_data_t *records, int count,
                                    meta_carve_t *out)
{
    if (records == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    if (count < 0 || count > META_BACKUP_DATA_MAX) return ESP_ERR_INVALID_ARG;

    // 清空输出
    memset(out, 0, sizeof(*out));
    out->data_count = (uint8_t)count;

    for (int i = 0; i < count; i++) {
        if (!meta_carve_data_valid(&records[i])) {
            // 静默跳过无效记录
            continue;
        }
        out->data[i] = records[i];
    }
    return ESP_OK;
}
