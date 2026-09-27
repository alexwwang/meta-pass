// main/meta_store_idedit.c —— 实现见头文件。纯逻辑,零依赖。
#include "meta_store_idedit.h"

#include <stdio.h>
#include <string.h>

// 网格行列(与头文件布局注释一致):
//   row0: 1 2 3 / row1: 4 5 6 / row2: 7 8 9
//   row3: CLR 0 OK / row4: DEL ◀ ▶
void mpd_key_row_col(int key, int *row, int *col)
{
    if (key < 0 || key >= MPD_KEY_COUNT) { *row = 0; *col = 0; return; }
    *row = key / 3;
    *col = key % 3;
}

static int key_from_row_col(int row, int col)
{
    return (row * 3 + col) % MPD_KEY_COUNT;
}

void mpd_idedit_init(mpd_idedit_t *e)
{
    memset(e, 0, sizeof(*e));
    e->sel = MPD_KEY_1;
}

// 数字键按下:光标处插入(右侧有空位时)。len==7 时拒绝(不自动提交)。
static bool press_digit(mpd_idedit_t *e, char d)
{
    if (e->len >= MPD_MAX_DIGITS || e->cursor > e->len) return false;
    memmove(e->digits + e->cursor + 1, e->digits + e->cursor,
            (size_t)(e->len - e->cursor));
    e->digits[e->cursor++] = d;
    e->len++;
    return true;
}

bool mpd_idedit_press(mpd_idedit_t *e)
{
    const int k = e->sel;
    if (k >= MPD_KEY_1 && k <= MPD_KEY_9) return press_digit(e, (char)('1' + k));
    if (k == MPD_KEY_0) return press_digit(e, '0');
    if (k == MPD_KEY_CLR) { mpd_idedit_clear(e); return true; }
    if (k == MPD_KEY_DEL) { mpd_idedit_backspace(e); return true; }
    if (k == MPD_KEY_LEFT)  { mpd_idedit_move_horiz(e, -1); return true; }   // 同物理◀
    if (k == MPD_KEY_RIGHT) { mpd_idedit_move_horiz(e, +1); return true; }   // 同物理▶
    if (k == MPD_KEY_OK) {
        // OK 在 OK 键:有内容才请求提交(空按无效,不置位)。
        if (e->len > 0) e->commit_req = true;
        return e->commit_req;
    }
    return false;
}

// 左右移动 = 移动插入光标(0..len),不是移动选中键 —— 选中键的移动只由
// 换行(长按)和屏上 ◀▶ 键按下承担。光标在 0 向左不动、在 len 向右不动。
void mpd_idedit_move_horiz(mpd_idedit_t *e, int dir)
{
    const int c = e->cursor + dir;
    if (c < 0 || c > e->len) return;
    e->cursor = c;
}

void mpd_idedit_move_row(mpd_idedit_t *e, int dir)
{
    int row, col;
    mpd_key_row_col(e->sel, &row, &col);
    row = (row + dir + 5) % 5;
    e->sel = key_from_row_col(row, col);
}

void mpd_idedit_backspace(mpd_idedit_t *e)
{
    if (e->cursor == 0) return;              // 光标在首位:无前格可删
    memmove(e->digits + e->cursor - 1, e->digits + e->cursor,
            (size_t)(e->len - e->cursor));
    e->cursor--;
    e->len--;
}

void mpd_idedit_clear(mpd_idedit_t *e)
{
    const int sel = e->sel;
    memset(e, 0, sizeof(*e));
    e->sel = sel;
}

void mpd_idedit_render(const mpd_idedit_t *e, char *buf, size_t cap)
{
    if (!buf || cap == 0) return;
    size_t w = 0;
    if (e->len == 0) {
        snprintf(buf, cap, "ID: -");
        return;
    }
    w += (size_t)snprintf(buf + w, cap - w, "ID: ");
    for (int i = 0; i < e->len && w < cap - 1; i++) {
        if (i == e->cursor && w < cap - 1) buf[w++] = '|';
        if (w < cap - 1) buf[w++] = e->digits[i];
    }
    if (e->cursor == e->len && w < cap - 1) buf[w++] = '|';   // 尾部光标
    buf[w] = '\0';
}

bool mpd_idedit_value(const mpd_idedit_t *e, uint32_t *out)
{
    if (e->len == 0) return false;
    uint32_t v = 0;
    for (int i = 0; i < e->len; i++) v = v * 10u + (uint32_t)(e->digits[i] - '0');
    *out = v;
    return true;
}
