// meta_store_info_page.c —— P2 详情页三形态纯逻辑(BUG-20,r10.9)。
// 实现见头文件注释;渲染与按键路由共用同一判定,host 测试钉死
// (tests/test_meta_store_info_page.c)。
#include "meta_store_info_page.h"

#include <string.h>

meta_store_info_kind_t meta_store_info_classify(const meta_store_analysis_t *a,
                                                bool job_failed)
{
    if (job_failed) return META_INFO_RETRY;      // analyze 作业失败:RETRY/BACK
    if (!a) return META_INFO_RETRY;              // 结果未产出:等待/可重试
    if (a->supported) return META_INFO_INSTALL;  // 含 custom-partitions 警告形态
    if (strcmp(a->reason, "unavailable") == 0) return META_INFO_RETRY;
    if (strchr(a->reason, ' ') != NULL) return META_INFO_RETRY;   // 设备传输分类句
    return META_INFO_FINAL;                      // not-found/too-large/...
}

int meta_store_info_row_count(meta_store_info_kind_t kind)
{
    return (kind == META_INFO_FINAL) ? 1 : 2;
}

const char *meta_store_info_row0_label(meta_store_info_kind_t kind)
{
    switch (kind) {
    case META_INFO_INSTALL: return "CONFIRM";
    case META_INFO_RETRY:   return "RETRY";
    default:                return "BACK";
    }
}

bool meta_store_info_ok_advances(meta_store_info_kind_t kind, int row)
{
    if (kind == META_INFO_INSTALL && row == 0) return true;   // → 槽位页
    if (kind == META_INFO_RETRY && row == 0) return true;     // → 重新 analyze
    return false;                                             // 其余 = BACK
}
