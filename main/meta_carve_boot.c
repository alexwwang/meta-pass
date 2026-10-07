// main/meta_carve_boot.c —— 见 meta_carve_boot.h。纯逻辑;flash 副作用在 hooks.c。
#include "meta_carve_boot.h"

#include <string.h>

meta_boot_table_verdict_t meta_carve_boot_decide(
    const uint8_t live[META_PT_SIZE], const meta_carve_rec_t *rec,
    int active_slot)
{
    meta_boot_table_verdict_t v = { META_BOOT_TABLE_RESTORE_SAFE,
                                    "unknown table and no committed carve",
                                    NULL };
    static uint8_t expected[META_PT_SIZE];
    if (!live) {
        return v;
    }

    // A committed carve is the durable allocation authority, but it is not
    // itself the normal runtime partition table.  Runtime views are derived:
    // launcher => no child DATA; deep-sleep child => only that child's DATA.
    if (rec != NULL && meta_carve_rec_validate(rec)) {
        bool ok = false;
        if (active_slot >= 0 && active_slot < (int)rec->carve.count &&
            rec->carve.slot[active_slot].kind == META_CARVE_KIND_APP) {
            ok = meta_pt_from_carve_active(&rec->carve,
                                           rec->carve.slot[active_slot].play_id,
                                           expected);
        } else if (active_slot < 0) {
            ok = meta_pt_from_carve_active(&rec->carve, 0, expected);
        }
        if (!ok) {
            v.action = META_BOOT_TABLE_RESTORE_SAFE;
            v.reason = "committed carve cannot materialize runtime view";
            return v;
        }
        if (meta_pt_equal(live, expected)) {
            v.action = META_BOOT_TABLE_PROCEED;
            v.reason = active_slot >= 0
                ? "live table matches active child runtime view"
                : "live table matches launcher runtime view";
            return v;
        }
        v.action = META_BOOT_TABLE_RESTORE_RECORD;
        v.reason = active_slot >= 0
            ? "live table differs from active child runtime view"
            : "live table differs from launcher runtime view";
        v.table = expected;
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
