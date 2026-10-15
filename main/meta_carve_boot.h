// main/meta_carve_boot.h —— bootloader hook 的分区表裁决纯逻辑(设计 §4.4)。
//
// 输入:当前 0x8000 表字节 + 最新合法 committed carve 记录(NULL = 无);
// 输出:放行 / 从记录重写 / 回写内置安全表。flash 写擦与 otadata 清除在
// hooks.c(静态门 tests/test_dynslot_hook_gate.py 钉住接线顺序:裁决必须先于
// otadata 单次会话策略,边界 B5)。
#pragma once

#include <stdint.h>

#include "meta_carve_store.h"

typedef enum {
    META_BOOT_TABLE_PROCEED = 0,      // live 表可接受,原样放行
    META_BOOT_TABLE_RESTORE_RECORD,   // 写回记录内嵌表(含迁移窗口补物化)
    META_BOOT_TABLE_RESTORE_SAFE,     // 写回内置安全表(store 死/未知表)
} meta_boot_table_action_t;

typedef struct {
    meta_boot_table_action_t action;
    const char *reason;               // 静态字符串,直接进 ESP_LOG
    const uint8_t *table;             // PROCEED 时 NULL;否则指向待写回的 0xC00 表字节
                                      // (RESTORE_RECORD → rec->table;RESTORE_SAFE →
                                      //  decide 内部静态缓冲,单线程 boot 路径复用)
} meta_boot_table_verdict_t;

// rec 为 NULL 或结构校验失败 → 按无记录处理:live ∈ {安全表, legacy 表} 放行,
// 否则 RESTORE_SAFE(§4.7 "child scribbles the table" / "store dead")。
meta_boot_table_verdict_t meta_carve_boot_decide(
    const uint8_t live[META_PT_SIZE], const meta_carve_rec_t *rec,
    int active_slot);

/* active_slot == -1: launcher/runtime view (no Child DATA).
 * active_slot >= 0: deep-sleep/trial child view; only that child's DATA
 * partitions are expected to be visible. The committed full carve table is
 * never used as the normal runtime view. */
