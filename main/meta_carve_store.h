// main/meta_carve_store.h —— store 分区内 carve 记录的 A/B 编解码(纯逻辑)。
//
// store 分区(fixed 0x35A000/0x6000,设计 §4.3;即便表里尚无 store 条目,
// boot hook 与启动器也按裸地址读写 —— 与既有 Wi-Fi 裸备份同一 esp_flash 手法):
//   扇区0  carve 记录 A
//   扇区1  carve 记录 B
//   扇区2  安全表副本(0xC00,余 0xFF)
//   扇区3  scratch(保留,全 0xFF)
//   扇区4..5  Wi-Fi 凭据备份(L6 迁移后的新家;原 0x35A000 裸区在迁移时先抄过来)
//
// 记录 v2(4KB 扇区,erase-before-write,未用字节保持 0xFF;M1/M5 数据 carve
// 随 dynslot 落地时从 v1 升级,v1 仍可解码兼容读 —— 已部署设备的记录不作废):
//   [0,16)     header: magic "MPSC" | version u16 | slot_count u16 | seq u32 |
//              data_count u16 | reserved u16(0xFF)
//   [16,752)   slot[8] × 92B: state u8 | kind u8 | reserved u16(0xFF) |
//              offset u32 | size u32 | image_len u32 | play_id u32 |
//              sha256[32] | name[40]
//   [752,3776) 内嵌 carved 表(0xC00,自带 MD5 marker)
//   [3776,4032) data[8] × 32B: play_id u32 | offset u32 | size u32 |
//              state u8 | subtype u8 | type u8 | reserved u8(0xFF) | label[16]
//   [4032,4036) crc32(覆盖 [0,4032),标准 CRC-32:初值 0xFFFFFFFF、
//              反射多项式 0xEDB88320、末异或,zlib/Python zlib.crc32 同值)
//   [4036,4096) 0xFF
//
// v1(只读兼容,老布局):slot 88B 无 play_id、无 data 区、crc @3792;
// 解码后 play_id=0、data_count=0(不与数据记录联动)。写出永远是 v2。
//
// CRC 放在最后写:撕裂写几乎必然落在未写的 CRC 上 → 解码即拒,
// 旧记录(A/B 另一扇区)胜出 —— §4.7 "mid store commit" 语义。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meta_carve.h"

#define META_STORE_OFFSET            0x35A000u
#define META_STORE_SECTOR_SIZE       0x1000u
#define META_STORE_SECTORS           6u
#define META_STORE_RECORD_A_SECTOR   0u
#define META_STORE_RECORD_B_SECTOR   1u
#define META_STORE_SAFE_TABLE_SECTOR 2u
#define META_STORE_SCRATCH_SECTOR    3u
#define META_STORE_CRED_SECTOR       4u   // L6:凭据备份新家(0x35E000)
#define META_STORE_CRED_SECTORS      2u

// 原裸凭据备份位置 = store 扇区0;迁移时先抄到 META_STORE_CRED_SECTOR 再写记录。
#define META_CRED_BAK_OFFSET (META_STORE_OFFSET + META_STORE_CRED_SECTOR * META_STORE_SECTOR_SIZE)

// 凭据备份 blob 的 magic "MPCK"(与 meta_store_net 的备份格式同源);
// ensure 的 cred_relocate 靠它区分扇区0 里是旧凭据还是记录/垃圾。
#define META_CRED_BAK_MAGIC  0x4B43504Du

#define META_CARVE_REC_MAGIC     0x4353504Du   // 'M','P','S','C'(小端)
#define META_CARVE_REC_VERSION   2u            // 当前写出版本(v1 只读兼容)
#define META_CARVE_REC_V1_VERSION 1u
#define META_CARVE_REC_SIZE      4096u
#define META_CARVE_REC_HEADER    16u
#define META_CARVE_REC_SLOT_SIZE 92u           // v2 槽位条目(+play_id)
#define META_CARVE_REC_DATA_SIZE 32u           // v2 数据条目
#define META_CARVE_REC_TABLE_OFF (META_CARVE_REC_HEADER + \
                                  META_CARVE_MAX_SLOTS * META_CARVE_REC_SLOT_SIZE)  /* 752 */
#define META_CARVE_REC_DATA_OFF  (META_CARVE_REC_TABLE_OFF + META_PT_SIZE)           /* 3776 */
#define META_CARVE_REC_CRC_OFF   (META_CARVE_REC_DATA_OFF + \
                                  META_DATA_MAX * META_CARVE_REC_DATA_SIZE)          /* 4032 */
// v1 老布局(解码兼容用,不再写出)。
#define META_CARVE_REC_V1_SLOT_SIZE 88u
#define META_CARVE_REC_V1_TABLE_OFF (META_CARVE_REC_HEADER + \
                                     META_CARVE_MAX_SLOTS * META_CARVE_REC_V1_SLOT_SIZE) /* 720 */
#define META_CARVE_REC_V1_CRC_OFF   (META_CARVE_REC_V1_TABLE_OFF + META_PT_SIZE)         /* 3792 */

typedef struct {
    uint32_t      seq;     // 单调递增(允许回绕);0/0xFFFFFFFF 视为无效
    meta_carve_t  carve;
    uint8_t       table[META_PT_SIZE];
} meta_carve_rec_t;

// 标准 CRC-32("123456789" == 0xCBF43926,黄金向量钉死)。
uint32_t meta_carve_crc32(const void *data, size_t len);

// 编码:参数非法(count>8 / carve 非法 / seq 空)→ false。
bool meta_carve_rec_encode(const meta_carve_rec_t *rec,
                           uint8_t out[META_CARVE_REC_SIZE]);

// 解码:magic/version/count/seq/CRC/表 MD5 逐层校验;name/label 强制 NUL。
// 版本分支:v1 老布局(无 play_id/无 data)也能读,物化成等价 v2 结构
// (play_id=0、data_count=0);v1 记录的表与 v2 物化结果逐字节相同。
// 只做格式层;结构一致性(carve↔表)在 validate。
// 实现直接物化 *out(不做 3.9KB 栈上临时记录 —— bootloader 栈小,
// §4.4);失败时 *out 内容未定义,调用方必须先判返回值再使用。
bool meta_carve_rec_decode(const uint8_t raw[META_CARVE_REC_SIZE],
                           meta_carve_rec_t *out);

// 轻校验 + seq 读取:magic/version/count/seq/CRC 逐层校验,不物化 carve/表
// (零栈开销)。供 hook “先比 seq、只对胜者跑完整 decode”用 —— bootloader
// BSS 只需持有一条记录(dynslot §4.4 dram_seg 预算)。不查表 MD5(那是
// decode 的活):CRC 过 + MD5 不过 → 此处放行、decode 拦截,语义不漏。
bool meta_carve_rec_raw_info(const uint8_t raw[META_CARVE_REC_SIZE],
                             uint32_t *seq_out);

// 结构校验:carve 合法 且 表 == 物化(carve)(防改记录不改表)。
bool meta_carve_rec_validate(const meta_carve_rec_t *rec);

// A/B 选取:两扇区各自解码+结构校验,seq 新者胜(有符号差回绕比较);
// 都无效 → false。*from_a 告知命中扇区(日志/诊断用)。
bool meta_carve_rec_pick(const uint8_t a[META_CARVE_REC_SIZE],
                         const uint8_t b[META_CARVE_REC_SIZE],
                         meta_carve_rec_t *out, bool *from_a);
