// meta_store_info_page.h —— P2 商店详情页三形态纯逻辑(BUG-20,r10.9)。
//
// BUG-20:r10.4 把渲染判定(store_info_is_retry_page)与按键路由(items 计算)
// 拆成了两处,重构后渲染器丢失 supported 分支 —— custom-partitions 警告页在
// 真机上只剩 BACK,CONFIRM 消失,警告+可安装的设计落空。此前真机从未到达过
// supported 页(TLS/状态码双杀在前),该分支在设备上零覆盖,host 也不测 LVGL
// 渲染,故潜伏至今。
//
// 本模块把三形态判定收成单一事实源,渲染与路由共用:
//   INSTALL : supported(含 custom-partitions 警告)→ CONFIRM/BACK,OK 行 0 = 进槽位页;
//   RETRY   : analyze 失败/未产出/unavailable/传输分类句 → RETRY/BACK,OK 行 0 = 重新 analyze;
//   FINAL   : 终态不支持(not-found/too-large/wrong-chip/...) → 仅 BACK。
#pragma once

#include <stdbool.h>

#include "meta_store_analysis.h"

typedef enum {
    META_INFO_INSTALL = 0,   // 可安装(CONFIRM/BACK)
    META_INFO_RETRY,         // 可重试失败(RETRY/BACK)
    META_INFO_FINAL,         // 终态不支持(仅 BACK)
} meta_store_info_kind_t;

// 页面形态判定。job_failed = analyze 作业以失败收场(meta_store_net_job 失败态);
// a = analyze 结果(可为 NULL = 未产出)。单一事实源。
meta_store_info_kind_t meta_store_info_classify(const meta_store_analysis_t *a,
                                                bool job_failed);

// 该形态渲染几行(1 或 2)。
int meta_store_info_row_count(meta_store_info_kind_t kind);

// 行 0 标签(CONFIRM/RETRY/BACK)。行 1 恒为 BACK(两行形态时)。
const char *meta_store_info_row0_label(meta_store_info_kind_t kind);

// OK 键语义:该形态下按 OK 于行 row 是否为"前进"动作(进槽位页/重试);
// false = BACK 语义(回输 ID 页)。
bool meta_store_info_ok_advances(meta_store_info_kind_t kind, int row);
