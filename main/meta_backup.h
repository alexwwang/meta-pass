// main/meta_backup.h —— M5 备份文件格式与验证（主机测试兼容版）
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// 备份文件头："MPTB" (Meta-Pass Tape Backup)
#define META_BACKUP_MAGIC 0x4254504DU  // 'MPTB' in little-endian
#define META_BACKUP_VERSION 1U

// 固件版本号最大长度（设计 §12.1）
#define META_BACKUP_VERSION_MAX 32

// 最大数据记录数（与 meta_carve.h DATA_MAX 一致）
#define META_BACKUP_DATA_MAX 8

// 错误码（主机测试兼容）
typedef enum {
    META_BACKUP_OK = 0,
    META_BACKUP_ERR_INVALID_ARG = -1,
    META_BACKUP_ERR_FORMAT = -2,
    META_BACKUP_ERR_SPACE = -3,
} meta_backup_err_t;

// 备份头结构（手机与设备共用布局）
typedef struct {
    uint32_t magic;                           // META_BACKUP_MAGIC
    uint32_t version;                         // META_BACKUP_VERSION
    uint32_t play_id;                         // 玩法 ID
    char firmware_version[META_BACKUP_VERSION_MAX];  // 固件版本字符串
    uint32_t data_count;                      // 后续 data[] 数组长度
} meta_backup_header_t;

// 单条数据记录（备份时序列化）
typedef struct {
    uint32_t play_id;
    uint32_t offset;                          // pool 内偏移
    uint32_t size;                            // 数据长度（已对齐到 4KB）
    uint8_t state;                            // META_DATA_ARCHIVED
    uint8_t type;                             // META_PT_TYPE_DATA
    uint8_t subtype;
    char label[17];                           // 16 + NUL
} meta_backup_data_t;

// 解析结果结构
typedef struct {
    bool ok;
    uint32_t play_id;
    char firmware_version[META_BACKUP_VERSION_MAX];
    uint32_t data_count;
    meta_backup_data_t records[META_BACKUP_DATA_MAX];
} meta_backup_result_t;

// 验证备份头基本结构
bool meta_backup_header_valid(const meta_backup_header_t *h);

// 比较固件版本（严格匹配）
// 返回 true = 匹配，false = 不匹配或格式错误
bool meta_backup_firmware_match(const meta_backup_header_t *h, const char *current_version);

// 序列化 carve 数据为备份记录
// 返回实际写入的记录数（0 = 无 ARCHIVED 数据）
int meta_backup_serialize(const uint32_t *play_ids, const uint8_t *states,
                          const uint32_t *offsets, const uint32_t *sizes,
                          const char **labels, int count,
                          meta_backup_data_t out[META_BACKUP_DATA_MAX]);

// 解析备份数据
meta_backup_result_t meta_backup_parse(const uint8_t *data, size_t len);

// 导入前的记录筛选(纯逻辑):只保留可导入的记录:
//   只取 ARCHIVED state=2、offset/size 非零、label 为空(建不出 carve 外
//   无法建条目)均剔除。写回 out 并返回保留条数。
// state 非 ARCHIVED / offset=0 / size=0 / label 为空 → 剔除(设计 §12.3)。
int meta_backup_filter_import(const meta_backup_data_t *in, int count,
                              meta_backup_data_t out[META_BACKUP_DATA_MAX]);

// 导入空间判定(纯逻辑,对应设计 §12.4):
//   total_needed = 0            → NO_RECORDS(无可导入内容,调用方映射 400)
//   free_bytes >= total_needed  → OK(直接导入)
//   reclaimable > 0             → NEED_ARC(先回收归档再重试)
//   否则                          → INSUFFICIENT(映射 507)
// free_bytes/reclaimable 均由调用方从规范 carve 现算。
typedef enum {
    META_IMPORT_OK = 0,
    META_IMPORT_ERR_NO_RECORDS,
    META_IMPORT_ERR_NEED_ARC,
    META_IMPORT_ERR_INSUFFICIENT,
} meta_import_verdict_t;

meta_import_verdict_t meta_backup_import_verdict(uint32_t free_bytes,
                                                 uint32_t total_needed,
                                                 uint32_t reclaimable);
