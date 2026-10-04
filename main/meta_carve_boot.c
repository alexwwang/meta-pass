// main/meta_carve_boot.c —— 见 meta_carve_boot.h。纯逻辑;flash 副作用在 hooks.c。
#include "meta_carve_boot.h"

#include <string.h>

meta_boot_table_verdict_t meta_carve_boot_decide(
    const uint8_t live[META_PT_SIZE], const meta_carve_rec_t *rec)
{
    meta_boot_table_verdict_t v = { META_BOOT_TABLE_RESTORE_SAFE,
                                    "unknown table and no committed carve",
                                    NULL };
    if (!live) {
        return v;
    }

    // 1) 有合法 committed 记录:字节比对,失配即权威修复(B5)。
    if (rec != NULL && meta_carve_rec_validate(rec)) {
        if (meta_pt_equal(live, rec->table)) {
            v.action = META_BOOT_TABLE_PROCEED;
            v.reason = "live table matches committed carve";
            return v;
        }
        v.action = META_BOOT_TABLE_RESTORE_RECORD;
        v.reason = "live table differs from committed carve";
        v.table = rec->table;
        return v;
    }

    // 2) 无记录(新设备 / store 死 / 记录结构坏):白名单 = 内置安全表或
    //    legacy v1.x 固定 3 槽表 —— legacy 必须放行,否则迁移前就被打回,
    //    已装玩法(池内)虽无损但 ota 条目丢失,§4.6 迁移将无从检测。
    //    静态参考缓冲:避免 boot 栈上 3KB 表缓冲(bootloader 栈小)。
    static uint8_t s_ref[META_PT_SIZE];
    meta_pt_safe(s_ref);
    if (meta_pt_equal(live, s_ref)) {
        v.action = META_BOOT_TABLE_PROCEED;
        v.reason = "safe table";
        return v;
    }
    meta_pt_legacy(s_ref);
    if (meta_pt_equal(live, s_ref)) {
        v.action = META_BOOT_TABLE_PROCEED;
        v.reason = "legacy v1.x table (migration pending)";
        return v;
    }
    v.action = META_BOOT_TABLE_RESTORE_SAFE;
    v.reason = "corrupt/foreign table and no committed carve";
    v.table = s_ref;   // 注意:此刻 s_ref 持有 legacy 字节 → 指回安全表。
    meta_pt_safe(s_ref);
    return v;
}
