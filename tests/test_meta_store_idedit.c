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

    // 长按 DOWN ×3 → 第四行 CLR;再 ×1 → 第五行 DEL;环绕回 1。
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_4);
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_7);
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_CLR);
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_DEL);
    mpd_idedit_move_row(&e, +1);
    assert(e.sel == MPD_KEY_1);

    // 长按 UP 从首行环绕到末行,列保持。
    mpd_idedit_move_row(&e, -1);
    assert(e.sel == MPD_KEY_DEL);
    // 列保持:从 OK(row3,col2) 上移 → 9(row2,col2)。
    e.sel = MPD_KEY_OK;
    mpd_idedit_move_row(&e, -1);
    assert(e.sel == MPD_KEY_9);
    printf("PASS: row navigation wraps and preserves column\n");
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

int main(void)
{
    test_insert_and_cursor();
    test_backspace();
    test_row_navigation();
    test_horiz_bounds();
    test_commit_and_capacity();
    printf("ALL META_STORE_IDEDIT TESTS PASSED\n");
    return 0;
}
