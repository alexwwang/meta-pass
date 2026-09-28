// main/meta_store_idedit.h —— P1 玩法 ID 编辑模型(纯逻辑,零 LVGL/IDF 依赖)。
// 真机(main.c)与 host 测试(tests/test_meta_store_idedit.c)链接同一份代码。
//
// 键盘布局(r9.1,4×4 网格,15 键;OK 占第四列下部两行):
//   1   2   3   DEL
//   4   5   6   CLR
//   7   8   9   OK(纵跨 r2-r3)
//   ◀   0   ▶   (OK 下半)
// ◀ ▶ 分列 0 的两侧(r9.1 用户指定);DEL/CLR/OK 独占第四列。
// 物理键映射(r10,main.c):UP/DOWN 短按 = 选中键沿环 ±1(可见高亮移动,
// 环绕可达全部 15 键);长按 = 换行(列保持,同列无键则停住 —— 第四列 row3
// 是 OK 的下半,换行到那里即停在 OK)。◀ ▶ 屏上键 = 移插入光标;DEL 退格;
// CLR 全清;OK 显式提交。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MPD_MAX_DIGITS 7   // 市场 id 校验 \d{1,7};第 7 位输满不自动提交(r9 交互:
                           // 用户可能输错,由 OK 显式提交,DEL 可退格修正)

// 键索引(与网格位置一致;main.c 据此摆放控件)。
enum {   // 行主序,与网格一一对应(4×4,OK 占 (2,3) 与 (3,3) 两格)
    MPD_KEY_1 = 0, MPD_KEY_2, MPD_KEY_3, MPD_KEY_DEL,
    MPD_KEY_4, MPD_KEY_5, MPD_KEY_6, MPD_KEY_CLR,
    MPD_KEY_7, MPD_KEY_8, MPD_KEY_9, MPD_KEY_OK,
    MPD_KEY_LEFT, MPD_KEY_0, MPD_KEY_RIGHT,
    MPD_KEY_COUNT,
};

// 网格几何:行/列(0..3;OK 返回其上格 (2,3))。
void mpd_key_row_col(int key, int *row, int *col);

typedef struct {
    char    digits[MPD_MAX_DIGITS];  // 已录数字('0'..'9')
    int     len;                     // 0..MPD_MAX_DIGITS
    int     cursor;                  // 插入光标 0..len(右侧可插)
    int     sel;                     // 当前高亮键 0..MPD_KEY_COUNT-1
    bool    commit_req;              // OK 在 GO/OK 键上按下且有内容 → 请求提交
} mpd_idedit_t;

// 初始化:空输入,光标在 0,选中 '1'。
void mpd_idedit_init(mpd_idedit_t *e);

// r10.7:预填数字(RETRY 回 P1 保留输入)。digits 仅 '0'..'9',1..7 位;
// 空/非法/超长返回 false 且不改变状态。成功后光标在末尾、选中键复位。
bool mpd_idedit_set_digits(mpd_idedit_t *e, const char *digits);

// 按下当前选中键。返回 true = 状态有变化(调用方刷新 UI);
// e->commit_req 置位后由调用方执行提交并调 mpd_idedit_clear。
bool mpd_idedit_press(mpd_idedit_t *e);

// 导航:dir=+1 右/-1 左(行内环绕,到行尾跳下一行行首/反之 —— 与整键盘
// 单向环绕不同,行内移动优先,只在行边界跨行,便于按行扫描)。
void mpd_idedit_move_horiz(mpd_idedit_t *e, int dir);

// 换行:dir=+1 下一行/-1 上一行(环绕),列位置尽量保持。
void mpd_idedit_move_row(mpd_idedit_t *e, int dir);

// 环移:选中键沿环 ±1(环序 = 枚举序 = 网格行主序),环绕可达全部 15 键。
// 物理短按 UP/DOWN 的可视导航;每按必动(不受第四列 row3 空格卡键影响)。
void mpd_idedit_move_ring(mpd_idedit_t *e, int dir);

// 语义动作(main.c 可直接绑定,测试全路径覆盖)。
void mpd_idedit_backspace(mpd_idedit_t *e);   // DEL:删光标前一格,光标左移
void mpd_idedit_clear(mpd_idedit_t *e);       // CLR/提交后:全清回初始态

// 渲染辅助:把 digits[0..len) 按 cursor 位置拆成 前|光标|后 三段写进 buf
// (光标以 '|' 表示;len==0 时显示 "-")。buf 建议 24 字节。
void mpd_idedit_render(const mpd_idedit_t *e, char *buf, size_t cap);

// 数值化(len==0 返回 false)。
bool mpd_idedit_value(const mpd_idedit_t *e, uint32_t *out);
