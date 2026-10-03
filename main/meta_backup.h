// main/meta_backup.h —— M5 备份文件格式与验证
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 备份文件头："MPTB" (Meta-Pass Tape Backup)
#define META_BACKUP_MAGIC 0x4254504DU  // 'MPTB' in little-endian
#define META_BACKUP_VERSION 1U

// 固件版本号最大长度（设计 §12.1）
#define META_BACKUP_VERSION_MAX 32

// 备份头结构（手机与设备共用布局）
typedef struct {
    uint32_t magic;                    // META_BACKUP_MAGIC
    uint32_t version;                  // META_BACKUP_VERSION
    uint32_t play_id;                  // 玩法 ID
    char firmware_version[META_BACKUP_VERSION_MAX];  // 固件版本字符串
    uint32_t data_count;               // 后续 data[] 数组长度
    // ... data[META_BACKUP_DATA_MAX] 紧随其后（变长）
} meta_backup_header_t;

// 单条数据记录（备份时序列化）
typedef struct {
    uint32_t play_id;
    uint32_t offset;                   // pool 内偏移
    uint32_t size;                     // 数据长度（已对齐到 4KB）
    uint8_t state;                     // META_DATA_ARCHIVED
    uint8_t type;                      // META_PT_TYPE_DATA
    uint8_t subtype;
    char label[17];                    // 16 + NUL
} meta_backup_data_t;

// 最大数据记录数（与 meta_carve.h DATA_MAX 一致）
#define META_BACKUP_DATA_MAX 8

// 备份文件总大小上限（header + 8 条记录）
#define META_BACKUP_HEADER_SIZE sizeof(meta_backup_header_t)
#define META_BACKUP_RECORD_SIZE sizeof(meta_backup_data_t)
#define META_BACKUP_MAX_SIZE (META_BACKUP_HEADER_SIZE + META_BACKUP_DATA_MAX * META_BACKUP_RECORD_SIZE)

// 验证备份头基本结构
bool meta_backup_header_valid(const meta_backup_header_t *h);

// 比较固件版本（严格匹配）
// 返回 true = 匹配，false = 不匹配或格式错误
bool meta_backup_firmware_match(const meta_backup_header_t *h, const char *current_version);

// 从 carve 记录序列化为备份数据
// 返回实际写入的记录数（0 = 无 ARCHIVED 数据）
int meta_backup_serialize(const meta_carve_t *carve, uint32_t play_id,
                          meta_backup_data_t out[META_BACKUP_DATA_MAX]);

// 从备份数据反序列化为 carve 记录
// 返回 ESP_OK 或错误码
esp_err_t meta_backup_deserialize(const meta_backup_data_t *records, int count,
                                   meta_carve_t *out);
