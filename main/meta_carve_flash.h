// main/meta_carve_flash.h —— dynslot 设备侧胶水:store 裸 flash 读写、启动
// 迁移/确保(ensure)、规范 carve 生命周期。只用 esp_flash_* 裸 API(不依赖
// 分区表缓存),因此:
//   - 必须在 app_main 任何分区 API 调用之前执行 ensure(IDF 分区表一次性
//     加载进 RAM,design §4.4);
//   - host 测试 tests/test_meta_carve_flash.c 用 RAM NOR 模型(AND 写语义 +
//     撕裂写注入)链接同一份源,覆盖 §4.7 失败矩阵的状态转换。
// bootloader hook 不用本模块(hook 直接读固定地址,见 meta_carve_boot.c)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "meta_carve.h"
#include "meta_carve_store.h"
#include "meta_slots.h"

// 0x8000 = IDF ESP_PARTITION_TABLE_OFFSET(裸偏移,host 侧无 IDF 头)。
#define META_PT_FLASH_OFFSET 0x8000u

// 启动早期执行一次(幂等):
//   - 有合法记录 → 采纳为规范 carve;live 表与记录不一致则防御性重写;
//   - 无记录 + live == legacy v1.x 表 → 迁移:先抄 Wi-Fi 凭据备份(原 0x35A000
//     就是 store 扇区0,记录会覆盖它,L6),再种子 carve、提交记录、物化表
//     (记录先于表:任一步断电都由 hook 下一开机补齐);live == 安全表 → 全新
//     设备,保持安全表直到首次 carve;
//   - 无记录 + 未知表 → 防御性写回内置安全表(hook 正常已处理,会话内兜底)。
esp_err_t meta_carve_flash_ensure(void);

// 规范 carve(ensure 前为全零空 carve)。安装/提案路径在此基础上改写。
const meta_carve_t *meta_carve_flash_carve(void);

// 规范 carve 是否已有 committed 记录(全新设备为 false)。
bool meta_carve_flash_has_record(void);

// 提交新 carve:A/B 扇区轮转写(写到非最新扇区,erase-before-write,CRC 收尾),
// materialize=true 时同步物化 0x8000 表(记录先写,表后写 —— 断电顺序安全)。
esp_err_t meta_carve_flash_commit(const meta_carve_t *carve, bool materialize);

// §4.5 Remove:从规范 carve 移除下标 slot(压缩数组,数据不搬移 —— 擦除由
// 网页重灌承担),提交记录并重新物化 0x8000 表(去条目)。
// 不在 carve 的下标 → ESP_ERR_INVALID_ARG,不写任何东西。
esp_err_t meta_carve_flash_remove(int slot);

// 扫描回填:把派生态(EMPTY/VALID/INVALID + sha + name)同步进记录;
// 无变化不写(磨损友好)。count 必须 == 规范 carve 槽数(同一张表派生)。
esp_err_t meta_carve_flash_sync_states(const meta_slot_info_t *slots, int count);

// 0x8000 表读写(裸 flash;写 = 擦 4KB 扇区 → 写 0xC00 → 读回比对)。
bool meta_carve_flash_table_read(uint8_t out[META_PT_SIZE]);
esp_err_t meta_carve_flash_table_write(const uint8_t table[META_PT_SIZE]);

// ---- M5 数据生命周期 -------------------------------------------------------

// 将指定 play_id 的所有 PRISTINE 数据记录翻为 DIRTY(安装成功后调用)。
// 幂等:已是 DIRTY 则空操作。返回 ESP_OK 或 flash 错误。
esp_err_t meta_carve_flash_set_dirty(uint32_t play_id);

// 卸载归档:将指定槽位对应的所有数据记录翻为 ARCHIVED(默认策略,不擦字节)。
// 调用 meta_carve_flash_remove 前使用此函数;后者只删槽位记录。
esp_err_t meta_carve_flash_archive_slot_and_data(int slot);

// 显式擦除指定 play_id+label 的数据记录(擦字节 + 移除记录)。
// 用于用户主动选择"删除数据"时的路径。
esp_err_t meta_carve_flash_erase_data(uint32_t play_id, const char *label);

// 池压力 ARC:从 ARCHIVED 数据记录中回收至多 target 字节(最旧优先)。
// 返回实际回收字节数(0 = 无可用归档或全部回收失败)。
uint32_t meta_carve_flash_arc(uint32_t target);

// host 测试专用:重置模块状态(模拟重启);真机不调用。
void meta_carve_flash_test_reset(void);
