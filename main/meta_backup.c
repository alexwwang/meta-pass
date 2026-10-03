// main/meta_backup.c —— M5 备份文件格式实现
#include "meta_backup.h"
#include <string.h>
#include <stdio.h>

bool meta_backup_header_valid(const meta_backup_header_t *h)
{
    if (!h) return false;
    if (h->magic != META_BACKUP_MAGIC) return false;
    if (h->version != META_BACKUP_VERSION) return false;
    if (h->play_id == 0) return false;
    if (h->data_count > META_BACKUP_DATA_MAX) return false;
    // firmware_version 必须是非空字符串
    if (strlen(h->firmware_version) == 0) return false;
    return true;
}

bool meta_backup_firmware_match(const meta_backup_header_t *h, const char *current_version)
{
    if (!meta_backup_header_valid(h)) return false;
    if (current_version == NULL || strlen(current_version) == 0) return false;
    return strcmp(h->firmware_version, current_version) == 0;
}

int meta_backup_serialize(const uint32_t *play_ids, const uint8_t *states,
                          const uint32_t *offsets, const uint32_t *sizes,
                          const char **labels, int count,
                          meta_backup_data_t out[META_BACKUP_DATA_MAX])
{
    if (!play_ids || !states || !offsets || !sizes || !out) return 0;
    if (count <= 0) return 0;

    int written = 0;
    for (int i = 0; i < count && written < META_BACKUP_DATA_MAX; i++) {
        if (states[i] != 2) continue;  // Only ARCHIVED (state=2)
        if (sizes[i] == 0) continue;

        out[written].play_id = play_ids[i];
        out[written].offset = offsets[i];
        out[written].size = sizes[i];
        out[written].state = states[i];
        out[written].type = 1;  // DATA
        out[written].subtype = 1;
        if (labels[i]) {
            strncpy(out[written].label, labels[i], sizeof(out[written].label) - 1);
            out[written].label[sizeof(out[written].label) - 1] = '\0';
        } else {
            out[written].label[0] = '\0';
        }
        written++;
    }
    return written;
}

meta_backup_result_t meta_backup_parse(const uint8_t *data, size_t len)
{
    meta_backup_result_t result = {0};

    // 最小长度检查
    if (len < sizeof(meta_backup_header_t)) {
        result.ok = false;
        return result;
    }

    // 解析头部
    const meta_backup_header_t *header = (const meta_backup_header_t *)data;
    if (!meta_backup_header_valid(header)) {
        result.ok = false;
        return result;
    }

    result.ok = true;
    result.play_id = header->play_id;
    strncpy(result.firmware_version, header->firmware_version,
            META_BACKUP_VERSION_MAX - 1);
    result.firmware_version[META_BACKUP_VERSION_MAX - 1] = '\0';
    result.data_count = header->data_count;

    // 解析数据记录
    size_t offset = sizeof(meta_backup_header_t);
    for (uint32_t i = 0; i < header->data_count && i < META_BACKUP_DATA_MAX; i++) {
        if (offset + sizeof(meta_backup_data_t) > len) {
            result.ok = false;
            return result;
        }
        const meta_backup_data_t *rec = (const meta_backup_data_t *)(data + offset);
        result.records[i] = *rec;
        offset += sizeof(meta_backup_data_t);
    }

    return result;
}
