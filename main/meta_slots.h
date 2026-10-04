// main/meta_slots.h —— 固件槽位注册表(纯逻辑,与 ESP-IDF 解耦)。
// 槽位的物理位置由 carve 决定(dynslot 设计 §4.1:每槽 subtype = 0x10 + 下标,
// 偏移在池内 first-fit 分配;legacy v1.x 三槽表由迁移种子等价接管)。
// 本模块只管"每个槽位的状态与元数据",分区读写由 meta_store(ESP-IDF 侧)完成。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define META_SLOT_COUNT      8    // dynslot 上限(§9.1 决策;IDF 硬上限 16 OTA 下标)
#define META_NAME_LEN        32   // 对齐 esp_app_desc_t.project_name 定长
#define META_VERSION_LEN     32   // 对齐 esp_app_desc_t.version 定长
#define META_SHA256_HEX_LEN  64   // SHA-256 的十六进制字符数

typedef enum {
    META_SLOT_EMPTY = 0,   // 无固件(槽位已擦除)
    META_SLOT_VALID,       // 校验通过,可启动
    META_SLOT_INVALID,     // 有内容但校验失败或写入未完成;不可启动,建议删除
} meta_slot_state_t;

typedef struct {
    meta_slot_state_t state;
    bool     signed_fw;                      // true = 通过 meta-pass 签名验签
    char     name[META_NAME_LEN + 1];            // 以 '\0' 结尾;EMPTY 时为空串
    char     version[META_VERSION_LEN + 1];
    uint32_t size;                               // 镜像字节数;EMPTY 时为 0
    char     sha256_hex[META_SHA256_HEX_LEN + 1]; // 全镜像 SHA-256,小写十六进制
} meta_slot_info_t;

// 重置为 EMPTY 并清空全部字段。
void meta_slot_clear(meta_slot_info_t *slot);

// 仅 VALID 可启动。
bool meta_slot_bootable(const meta_slot_info_t *slot);

// 写入校验通过的元数据并把状态置为 VALID。
// name/version 超长、sha256_hex 非恰好 64 位小写十六进制 → 返回 false,slot 不变。
bool meta_slot_set_valid(meta_slot_info_t *slot, const char *name, const char *version,
                         uint32_t size, const char *sha256_hex);

// 标记 INVALID(保留元数据供界面展示"坏固件",但禁止启动)。
void meta_slot_mark_invalid(meta_slot_info_t *slot);

// r10:槽位状态的 UI 措辞单一事实源(main.c 直接渲染,host 测试钉死)。
// "(empty)" = 全 0xFF 已擦除;"(no firmware)" = 有数据但非可引导镜像
// (典型:ota_2 双用途区装过 littlefs 录音)。不用 "invalid" —— 歧义暗示
// 槽位损坏;安装路径 esp_ota_begin 先擦除,两种槽位都可直接覆盖安装。
const char *meta_slot_list_word(meta_slot_state_t state);   // 列表页单词形态
const char *meta_slot_detail_word(meta_slot_state_t state); // 详情页两行形态

// 派生槽位列表用的"核心名":剥掉常见 "FoloToy-"/"FoloToy_"/"FoloToy " 前缀
// (固件编译期 project_name 普遍带此前缀,小屏列表显示不下)。无前缀或剥完为空
// → 返回原名;slot 为 NULL → 返回空串。返回 slot->name 内部指针,零拷贝。
const char *meta_slot_core_name(const meta_slot_info_t *slot);
