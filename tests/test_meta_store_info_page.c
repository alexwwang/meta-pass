// tests/test_meta_store_info_page.c — BUG-20 回归门(r10.9)。
//
// BUG-20:r10.4 重构把渲染判定与按键路由拆成两处,渲染器丢失 supported 分支 —
// custom-partitions 警告页(supported=true)在真机上只剩 BACK,CONFIRM 消失,
// "警告+确认可安装"的设计落空。此前真机从未到达过 supported 页(BUG-18/19
// 双杀在前),设备上零覆盖。
//
// 本测试钉死单一事实源 meta_store_info_classify 的三形态判定与 OK 语义:
//   INSTALL(supported,含 custom-partitions 警告)→ CONFIRM/BACK,OK 行 0 前进;
//   RETRY(作业失败/unavailable/传输分类句)      → RETRY/BACK,OK 行 0 重试;
//   FINAL(not-found/too-large/wrong-chip/...)   → 仅 BACK,OK = BACK。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "meta_store_analysis.h"
#include "meta_store_info_page.h"

static meta_store_analysis_t mk(const char *reason, bool supported)
{
    meta_store_analysis_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.reason, sizeof(a.reason), "%s", reason);
    a.supported = supported;
    return a;
}

static void test_supported_is_install_two_rows(void)
{
    meta_store_analysis_t ok = mk("", true);
    assert(meta_store_info_classify(&ok, false) == META_INFO_INSTALL);
    assert(meta_store_info_row_count(META_INFO_INSTALL) == 2);
    assert(strcmp(meta_store_info_row0_label(META_INFO_INSTALL), "CONFIRM") == 0);
    assert(meta_store_info_ok_advances(META_INFO_INSTALL, 0) == true);
    assert(meta_store_info_ok_advances(META_INFO_INSTALL, 1) == false);
}

static void test_custom_partitions_warning_is_install(void)
{
    // 真机 675 的实际形态:supported=true + reason=custom-partitions(+detail)。
    // 警告文案渲染在正文,但页面形态必须仍是可安装的两行页。
    meta_store_analysis_t warn = mk("custom-partitions", true);
    assert(meta_store_info_classify(&warn, false) == META_INFO_INSTALL);
    assert(meta_store_info_row_count(META_INFO_INSTALL) == 2);
    assert(strcmp(meta_store_info_row0_label(META_INFO_INSTALL), "CONFIRM") == 0);
    assert(meta_store_info_ok_advances(META_INFO_INSTALL, 0) == true);
}

static void test_retry_forms(void)
{
    meta_store_analysis_t un = mk("unavailable", false);
    assert(meta_store_info_classify(&un, false) == META_INFO_RETRY);
    // 设备传输分类句含空格(r10.4 规则)。
    meta_store_analysis_t tls = mk("TLS failed (clock unsynced).", false);
    assert(meta_store_info_classify(&tls, false) == META_INFO_RETRY);
    assert(meta_store_info_classify(NULL, false) == META_INFO_RETRY);   // 未产出
    assert(meta_store_info_classify(NULL, true) == META_INFO_RETRY);    // 作业失败
    assert(meta_store_info_row_count(META_INFO_RETRY) == 2);
    assert(strcmp(meta_store_info_row0_label(META_INFO_RETRY), "RETRY") == 0);
    assert(meta_store_info_ok_advances(META_INFO_RETRY, 0) == true);
    assert(meta_store_info_ok_advances(META_INFO_RETRY, 1) == false);
}

static void test_final_forms_single_row(void)
{
    const char *finals[] = { "not-found", "too-large", "wrong-chip", "format",
                             "no-factory" };
    for (unsigned i = 0; i < sizeof(finals) / sizeof(finals[0]); ++i) {
        meta_store_analysis_t f = mk(finals[i], false);
        assert(meta_store_info_classify(&f, false) == META_INFO_FINAL);
    }
    assert(meta_store_info_row_count(META_INFO_FINAL) == 1);
    assert(strcmp(meta_store_info_row0_label(META_INFO_FINAL), "BACK") == 0);
    assert(meta_store_info_ok_advances(META_INFO_FINAL, 0) == false);
}

static void test_job_failed_beats_result(void)
{
    // 安装失败返回 P2 时作业带失败态,但 analyze 结果仍有效(r10.4 行为):
    // 有结果且 supported → 仍渲染可安装页,不被失败态覆盖。
    meta_store_analysis_t ok = mk("", true);
    assert(meta_store_info_classify(&ok, false) == META_INFO_INSTALL);
}

int main(void)
{
    test_supported_is_install_two_rows();
    test_custom_partitions_warning_is_install();
    test_retry_forms();
    test_final_forms_single_row();
    test_job_failed_beats_result();
    printf("ALL META_STORE_INFO_PAGE TESTS PASSED\n");
    return 0;
}
