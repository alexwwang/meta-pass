// main/meta_store_idedit.c —— 实现见头文件。纯逻辑,零依赖。
#include "meta_store_idedit.h"

#include <stdio.h>
#include <string.h>

// 网格表(4×4,r9.1 布局;OK 占 (2,3)+(3,3),表中记上格):
static const struct { uint8_t row, col; } k_grid[MPD_KEY_COUNT] = {
    {0,0},{0,1},{0,2},{0,3},
    {1,0},{1,1},{1,2},{1,3},
    {2,0},{2,1},{2,2},{2,3},
    {3,0},{3,1},{3,2},
};

void mpd_key_row_col(int key, int *row, int *col)
{
    if (key < 0 || key >= MPD_KEY_COUNT) { *row = 0; *col = 0; return; }
    *row = k_grid[key].row;
    *col = k_grid[key].col;
}

// 同列找目标行的键;没有(第四列 row3 是 OK 下半)返回 -1,调用方停在原地。
static int key_at(int row, int col)
{
    for (int i = 0; i < MPD_KEY_COUNT; i++) {
        if (k_grid[i].row == row && k_grid[i].col == col) return i;
    }
    return -1;
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
    // 数字键映射:枚举序是网格行主序(1 2 3 DEL / 4 5 6 CLR / 7 8 9 0),
    // 不能用 ('1'+k) 推 —— 显式查表。r9.1 第一版测试抓到此错(输 675 得 796)。
    static const char k_digit[MPD_KEY_COUNT] = {
        '1', '2', '3', 0,
        '4', '5', '6', 0,
        '7', '8', '9', 0,
        0,   '0', 0,
    };
    if (k_digit[k]) return press_digit(e, k_digit[k]);
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
    const int nr = (row + dir + 4) % 4;
    const int k = key_at(nr, col);
    if (k >= 0) e->sel = k;   // 同列无键(OK 下半)则停住
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
