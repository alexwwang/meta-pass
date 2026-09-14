// main/meta_store.h —— 固件槽位的设备侧存储层(ESP-IDF 依赖集中在此)。
// 纯逻辑(状态/元数据校验)在 meta_slots/meta_image;本文件只做分区 IO 与权威校验。
#pragma once

#include "meta_sign.h"
#include "meta_slots.h"

#include "esp_err.h"
#include "esp_partition.h"

// 扫描全部槽位填充 out:全 0xFF → EMPTY;esp_image_verify(含校验和/哈希)失败 → INVALID;
// 通过则读 esp_app_desc 的名称/版本、镜像长度,并流式计算全镜像 SHA-256 → VALID。
// 每次启动执行一次;3 槽的 flash 读取耗时在可接受范围(毫秒级×百次)。
esp_err_t meta_store_scan(meta_slot_info_t out[META_SLOT_COUNT]);

// 整槽擦除。slot 越界或分区不存在返回 ESP_ERR_INVALID_ARG。
esp_err_t meta_store_erase_slot(int slot);

// 把启动分区设为指定槽位(调用方负责随后 esp_restart())。
// 前置:槽位 meta_slot_bootable() 必须为真。
esp_err_t meta_store_boot_slot(int slot);

// 槽位分区句柄(供 meta_net 流式写入与容量查询)。失败返回 NULL。
const esp_partition_t *meta_store_slot_partition(int slot);

// 让 factory(启动器自身)成为永久有效的回滚目标:首启后标记当前 factory 镜像有效。
// 幂等;非待验证状态时返回 ESP_ERR_INVALID_STATE,属正常情况,调用方按日志处理即可。
esp_err_t meta_store_mark_factory_valid(void);

// 惰性读取槽位彩蛋文本:定位 tail sector 并解析 MAEG(meta_sign.h)。
// image_len 取槽位注册表的 size 字段;装下最长文本需 out_cap >= META_EGG_TEXT_LEN+1。
// 调用方需 #include "meta_sign.h" 以获得 meta_egg_result_t。
meta_egg_result_t meta_store_read_egg(int slot, uint32_t image_len, char *out, size_t out_cap);
