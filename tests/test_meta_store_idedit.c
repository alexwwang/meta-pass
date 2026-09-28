// tests/test_meta_store_idedit.c —— P1 编辑模型 host 单测(与真机同一份代码)。
// 锁定交互契约:插入光标/退格/移位/换行/提交请求;并重放旧实现(无光标、固定
// 追加、无退格)会犯的错,防回退。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "meta_store_idedit.h"

static void test_insert_and_cursor(void)
{
    mpd_idedit_t e;
    mpd_idedit_init(&e);

    // 6 7 5 → ID: 675,光标在尾。
    e.sel = MPD_KEY_6; assert(mpd_idedit_press(&e));
    e.sel = MPD_KEY_7; assert(mpd_idedit_press(&e));
    e.sel = MPD_KEY_5; assert(mpd_idedit_press(&e));
    char buf[24];
    mpd_idedit_render(&e, buf, sizeof(buf));
    assert(strcmp(buf, "ID: 675|") == 0);

    // ◀ ◀ 移到 6[7|5] 之间,插 0 → 6075(中间插入,非追加)。
    mpd_idedit_move_horiz(&e, -1);
    mpd_idedit_move_horiz(&e, -1);
    e.sel = MPD_KEY_0;
    assert(mpd_idedit_press(&e));
    mpd_idedit_render(&e, buf, sizeof(buf));
    assert(strcmp(buf, "ID: 60|75") == 0);
    uint32_t v;
    assert(mpd_idedit_value(&e, &v) && v == 6075);
    printf("PASS: insert-mode editing with cursor (6075, not append)\n");
}

static void test_backspace(void)
{
    mpd_idedit_t e;
    mpd_idedit_init(&e);
    e.sel = MPD_KEY_6; mpd_idedit_press(&e);
    e.sel = MPD_KEY_1; mpd_idedit_press(&e);
    e.sel = MPD_KEY_2; mpd_idedit_press(&e);
    assert(e.len == 3);

    // DEL:删 2 → 61,光标随动。
    e.sel = MPD_KEY_DEL;
    assert(mpd_idedit_press(&e));
    assert(e.len == 2 && e.digits[0] == '6' && e.digits[1] == '1' && e.cursor == 2);

    // 再 DEL ×2 → 空,再 DEL 无效果(首位光标)。
    mpd_idedit_press(&e);
    mpd_idedit_press(&e);
    assert(e.len == 0 && e.cursor == 0);
    mpd_idedit_press(&e);
    assert(e.len == 0);
    char buf[24];
    mpd_idedit_render(&e, buf, sizeof(buf));
    assert(strcmp(buf, "ID: -") == 0);
    printf("PASS: backspace removes before cursor, safe at empty\n");
}

static void test_row_navigation(void)
{
    mpd_idedit_t e;
    mpd_idedit_init(&e);   // sel = 1 (row0,col0)

    // 第一列换行:1 → 4 → 7 → ◀ → 环绕回 1。
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_4);
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_7);
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_LEFT);
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_1);

    // 第四列:DEL → CLR → OK →(OK 下半无键,停住)。
    e.sel = MPD_KEY_DEL;
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_CLR);
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_OK);
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_OK);

    // 列保持上移:OK 上格(row2,col3) → CLR(row1,col3);环绕 0(col1) 上移 → 8。
    e.sel = MPD_KEY_OK;
    mpd_idedit_move_row(&e, -1);
    assert(e.sel == MPD_KEY_CLR);
    e.sel = MPD_KEY_0;
    mpd_idedit_move_row(&e, -1);
    assert(e.sel == MPD_KEY_8);
    // 环绕:0(row3) 下移 → 2(row0,col1)。
    e.sel = MPD_KEY_0;
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_2);
    printf("PASS: row navigation wraps and preserves column\n");
}

// r10:物理 UP/DOWN 短按 = 选中键沿环 ±1(环序 = 枚举序 = 网格行主序)。
// 回归:中间版本把短按映射到移光标,键盘高亮只剩长按换行一条路,列方向
// 永远动不了 —— 环移测试钉死"短按必须移动可见的高亮键"。
static void test_ring_navigation(void)
{
    mpd_idedit_t e;
    mpd_idedit_init(&e);   // sel = MPD_KEY_1(环首)

    // 正向一整圈:+1 ×15 回到起点,途中逐键命中全部 15 键。
    const int ring[MPD_KEY_COUNT] = {
        MPD_KEY_1, MPD_KEY_2, MPD_KEY_3, MPD_KEY_DEL,
        MPD_KEY_4, MPD_KEY_5, MPD_KEY_6, MPD_KEY_CLR,
        MPD_KEY_7, MPD_KEY_8, MPD_KEY_9, MPD_KEY_OK,
        MPD_KEY_LEFT, MPD_KEY_0, MPD_KEY_RIGHT,
    };
    for (int i = 0; i < MPD_KEY_COUNT; i++) {
        assert(e.sel == ring[i]);
        mpd_idedit_move_ring(&e, +1);
    }
    assert(e.sel == MPD_KEY_1);   // 环绕回环首

    // 反向:先移再断言(环首 -1 = 环尾),走一整圈逐键回环首。
    for (int i = MPD_KEY_COUNT - 1; i >= 0; i--) {
        mpd_idedit_move_ring(&e, -1);
        assert(e.sel == ring[i]);
    }
    // 环首再 -1:落在环尾(环绕无缝)。
    mpd_idedit_move_ring(&e, -1);
    assert(e.sel == ring[MPD_KEY_COUNT - 1]);

    printf("PASS: ring navigation covers all 15 keys both directions\n");
}

static void test_horiz_bounds(void)
{
    mpd_idedit_t e;
    mpd_idedit_init(&e);
    mpd_idedit_move_horiz(&e, -1);   // 光标在 0:左移无效
    assert(e.cursor == 0);
    e.sel = MPD_KEY_1; mpd_idedit_press(&e);
    e.sel = MPD_KEY_2; mpd_idedit_press(&e);
    assert(e.cursor == 2);
    mpd_idedit_move_horiz(&e, +1);   // 光标在 len:右移无效
    assert(e.cursor == 2);
    mpd_idedit_move_horiz(&e, -2);   // 回 0
    assert(e.cursor == 0);
    printf("PASS: cursor movement bounded [0,len]\n");
}

static void test_commit_and_capacity(void)
{
    mpd_idedit_t e;
    mpd_idedit_init(&e);

    // 空按 OK:无提交请求。
    e.sel = MPD_KEY_OK;
    assert(!mpd_idedit_press(&e) && !e.commit_req);

    // 输满 7 位:第 7 位不再自动提交(旧实现的回归点),继续按键被拒。
    mpd_idedit_clear(&e);
    e.sel = MPD_KEY_7;
    const int seven_keys[] = { MPD_KEY_1, MPD_KEY_2, MPD_KEY_3, MPD_KEY_4,
                               MPD_KEY_5, MPD_KEY_6, MPD_KEY_7 };
    for (size_t i = 0; i < 7; i++) {
        e.sel = seven_keys[i];
        assert(mpd_idedit_press(&e));
    }
    assert(e.len == 7 && !e.commit_req);
    e.sel = MPD_KEY_8;
    assert(!mpd_idedit_press(&e) && e.len == 7);

    // OK 显式提交 → commit_req 置位;clear 复位保留选中。
    e.sel = MPD_KEY_OK;
    assert(mpd_idedit_press(&e) && e.commit_req);
    mpd_idedit_clear(&e);
    assert(e.len == 0 && !e.commit_req && e.sel == MPD_KEY_OK);
    uint32_t v;
    assert(!mpd_idedit_value(&e, &v));
    printf("PASS: explicit commit only, 7-digit capacity respected\n");
}

// r10.7:预填与导出——RETRY 不清输入的设备端语义在模型层的根据:
// set_digits 拒绝空串/非法字符/超长(非法输入不动原状态),value 空时拒绝导出。
static void test_preset_and_export(void)
{
    mpd_idedit_t e;
    mpd_idedit_init(&e);

    // 非法预填一概拒绝且不动原状态
    assert(!mpd_idedit_set_digits(&e, ""));
    assert(e.len == 0);
    assert(!mpd_idedit_set_digits(&e, "12a"));
    assert(e.len == 0);
    assert(!mpd_idedit_set_digits(&e, "12345678"));   // 超过 7 位上限
    assert(e.len == 0);

    // 合法预填:数字落位,光标在末尾(续输追加),选中键复位到 '1'
    assert(mpd_idedit_set_digits(&e, "563"));
    assert(e.len == 3 && e.digits[0] == '5' && e.digits[1] == '6'
           && e.digits[2] == '3' && e.cursor == 3 && e.sel == MPD_KEY_1);

    // 续输追加:预填 563 后按 7 → 5637
    e.sel = MPD_KEY_7;
    assert(mpd_idedit_press(&e));
    assert(e.len == 4 && e.digits[3] == '7');

    // 重复预填覆盖(不改光标语义,仍在末尾)
    assert(mpd_idedit_set_digits(&e, "9"));
    assert(e.len == 1 && e.digits[0] == '9' && e.cursor == 1);

    // 导出:有数字时成功;空时拒绝且不写 out
    uint32_t v = 12345;
    assert(mpd_idedit_value(&e, &v) && v == 9);
    mpd_idedit_clear(&e);
    v = 777;
    assert(!mpd_idedit_value(&e, &v) && v == 777);

    printf("PASS: preset (set_digits) + export (value) semantics\n");
}

int main(void)
{
    test_insert_and_cursor();
    test_backspace();
    test_row_navigation();
    test_ring_navigation();
    test_horiz_bounds();
    test_commit_and_capacity();
    test_preset_and_export();
    printf("ALL META_STORE_IDEDIT TESTS PASSED\n");
    return 0;
}
