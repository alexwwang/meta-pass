// main/main.c —— meta-pass 多固件启动器:槽位管理 + 商店下载安装 + 引导。
//
// 设计文档(单一权威来源):docs/assets/meta-pass-design.md;商店下载通道见
// main/meta_store_net.h / meta_store_api.h(方案:meta-pass网络下载改造方案)。
//
// 商店流程(P0..P5 + P4b 取消确认,数字键盘三键交互):
//   P0 配网  SoftAP 表单收 WiFi 凭证(或复用已存凭证直连)→ SNTP → ONLINE
//   P1 输ID  UP/DOWN 调当前位数字,OK 跳下一位;6 位输完自动取 analyze
//   P2 详情  名称 / 可装性 / 建议最小槽位(不支持时显示原因码),CONFIRM 继续
//   P3 选槽  三个槽位行,本地按分区上限标记 fit;仅可装槽位可确认
//   P4 下载  流式进度 + 服务端摘要双重校验,失败作废槽位
//   P4b 取消 下载中 OK LONG 进确认页:CANCEL=取消 / RETRY=重试 / BACK=继续等
//   P5 提示  安装完成,提示断电重启选择子固件
//
// 按键语义(全局统一):
//   上/下 短按   列表/详情页=移动选中项;启动确认页=切换 BOOT/CANCEL;彩蛋页=滚动文本;
//   上/下 连按   详情页/启动确认页快速四连按 UP UP DOWN DOWN=进入彩蛋页(PRESS 判定,
//                相邻两键间隔 <500ms,见 meta_seq.h)
//   确定  短按   进入/确认菜单项;未签名启动警告页=执行所选动作
//   确定  长按   返回上一级(LONG,1.5s);例外:删除确认页 OK 长按=确认删除
//   注意:切换子固件靠 Power 关机重启,meta-pass 不干预子固件的按键行为
#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "meta_seq.h"
#include "meta_slots.h"
#include "meta_store.h"
#include "meta_store_api.h"
#include "meta_store_net.h"
#include "meta_store_idedit.h"
#include "meta_store_prov.h"
#include "ui_pixel.h"

static const char *TAG = "meta-pass";

// 页面枚举:每个页面独立 build/teardown(沿用基线 demo 的建删屏纪律)。
typedef enum {
    PAGE_LIST = 0,     // 槽位列表 + Store 入口
    PAGE_DETAIL,       // 槽位详情:Boot / Delete / Back
    PAGE_CONFIRM_BOOT, // 未签名固件启动警告
    PAGE_CONFIRM_DEL,  // 删除确认
    PAGE_EGG,          // 彩蛋页:隐藏序列进入,滚动查看 MAEG 文本
    PAGE_STORE_NET,    // P0 商店:配网/连接状态
    PAGE_STORE_ID,     // P1 商店:数字键盘输入玩法 ID
    PAGE_STORE_INFO,   // P2 商店:详情(名称/可装性/建议槽位)
    PAGE_STORE_SLOT,   // P3 商店:目标槽位选择
    PAGE_STORE_DL,     // P4 商店:下载+刷写进度
    PAGE_STORE_CANCEL, // P4b 商店:取消下载确认(CANCEL/RETRY/BACK,后台下载不停)
    PAGE_STORE_DONE,   // P5 商店:安装完成提示
} page_t;

#define LIST_ITEMS   4                   // Slot 0 / Slot 1 / Slot 2 / Store
#define DETAIL_ITEMS 3                   // Boot / Delete / Back
// 商店会话超时不再"到点即关":到期提示用户决策(OK=保留 / LONG=退出),见 store_tick。
// 超时时长默认 CONFIG_META_STORE_SESSION_TIMEOUT_MS,运行时可用
// meta_store_session_set_timeout_ms 覆盖(见 meta_store_net.h)。

static meta_slot_info_t s_slots[META_SLOT_COUNT];  // 槽位注册表(安装成功也回写它)

static page_t    s_page = PAGE_LIST;
static int       s_sel;              // 当前页选中行
static int       s_detail_slot;      // 详情/确认页操作的槽位
static int64_t   s_store_deadline;   // 商店会话自动关闭时刻(ms,esp_timer 时基)
static lv_timer_t *s_store_timer;    // 商店页轮询定时器(离开页面前必须删)

static lv_obj_t *s_scr;              // 当前页 screen;同一时间只有一个
static lv_obj_t *s_rows[LIST_ITEMS]; // 可选中行面板(数量按页面上限分配)
static lv_obj_t *s_info;             // 详情/商店页的多行文本
static lv_obj_t *s_status_line;      // 商店页状态行
static lv_obj_t *s_egg_panel;        // 彩蛋页可滚动面板(teardown 时随屏销毁)
static lv_obj_t *s_mascot;
static meta_seq_state_t s_egg_seq;   // 详情页隐藏序列 UP UP DOWN DOWN(四 CLICK)的匹配状态

// ---- 商店会话状态(UI 上下文;网络侧状态在 meta_store_net 快照里) ----
static uint32_t s_play_id;                       // P1 确认的玩法编号
static lv_obj_t *s_keys[MPD_KEY_COUNT];          // P1 键盘面板(全部 15 键)
// BUG-14:s_keys 曾硬编码 [10](r8 十键键盘遗留),r10 键盘扩到 15 键后每入
// P1 越界写 5 槽 —— map 实证后 20B 正好砸碎 s_rows[0..3] 与 s_slots[0]
// 头部,退出商店后列表页高亮/按键全废。修复 = 尺寸单一事实源(声明即用
// MPD_KEY_COUNT),键盘键数再变时数组自动跟随,不再可能静默越界。
static bool     s_slot_fit[META_SLOT_COUNT];     // P3 各槽位 fit 标记(本地分区上限)
static int      s_store_installed_slot;          // P3 确认的目标槽位(P5 展示用;store_goto 会清 s_sel)
static char     s_store_warn_detail[48];         // reason 的 detail:分区名(r9)或失败层位句(r10.4)
static bool     s_store_expired;                 // 会话已到期,等待用户决策(冻结自动迁移)
static lv_obj_t *s_timeout_panel;                // 到期提示浮层本体(ui_pixel_panel 立体框)
static lv_obj_t *s_timeout_lbl;                  // 浮层标签(s_timeout_panel 子对象)
static bool     s_store_retry;                   // P4b 选了 RETRY:当前下载取消后自动重新安装

// ---------- 公共小部件 ----------

// 右上角电量:读数 -1(不可用)时不画,避免显示假数字;位置在白云(188,8)下方的空闲蓝天区。
static void add_battery(lv_obj_t *parent)
{
    const int soc = bsp_battery_soc();
    char text[12];
    // 读数 -1(不可用)时不画,避免用未初始化缓冲区显示垃圾并触发越界读。
    if (soc < 0) return;
    snprintf(text, sizeof(text), "%d%%", soc);
    lv_obj_t *lbl = ui_pixel_label(parent, text, &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(lbl, 204, 30);
}

// 可选中行;selected 高亮。行文本随后用 lv_label_set_text 更新。
static lv_obj_t *add_row(lv_obj_t *parent, int idx, int y, const char *text)
{
    lv_obj_t *panel = ui_pixel_panel_create(parent, 12, y, 216, 40, UI_PAPER);
    lv_obj_t *lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_INK), 0);
    lv_obj_center(lbl);
    lv_label_set_text(lbl, text);
    s_rows[idx] = panel;
    return panel;
}

static void rows_refresh(int count, int sel)
{
    for (int i = 0; i < count; i++) {
        ui_pixel_set_selected(s_rows[i], i == sel, true);
    }
}

// 关闭当前页:先停定时器(防悬挂回调访问已删对象),再删屏、清空指针。
static void page_teardown(void)
{
    if (s_store_timer) {
        lv_timer_delete(s_store_timer);
        s_store_timer = NULL;
    }
    if (s_page >= PAGE_STORE_NET && s_page <= PAGE_STORE_DONE) {
        meta_store_net_stop();   // 完整释放 httpd/wifi(资源纪律见 meta_store_net.c)
    }
    if (s_scr) {
        lv_obj_delete(s_scr);   // 到期提示浮层是 s_scr 子对象,随屏一起销毁
        s_scr = NULL;
        s_info = NULL;
        s_status_line = NULL;
        s_egg_panel = NULL;
        s_mascot = NULL;
        s_timeout_panel = NULL;   // 浮层与影子都是 s_scr 子对象,随屏一起销毁
        s_timeout_lbl = NULL;
        for (int i = 0; i < LIST_ITEMS; i++) s_rows[i] = NULL;
    }
}

// ---------- 页面:槽位列表 ----------

static void list_refresh(void)
{
    for (int i = 0; i < META_SLOT_COUNT; i++) {
        lv_obj_t *lbl = lv_obj_get_child(s_rows[i], 0);
        char text[48];
        switch (s_slots[i].state) {
        case META_SLOT_VALID:
            snprintf(text, sizeof(text), "SLOT %d: %.20s", i, meta_slot_core_name(&s_slots[i]));
            break;
        case META_SLOT_INVALID:
            // r10 措辞:有数据但非可引导固件 —— 单一事实源 meta_slots.c
            // (host 测试钉死;安装 esp_ota_begin 先擦除,槽位完全可复用)。
            snprintf(text, sizeof(text), "SLOT %d: %s", i,
                     meta_slot_list_word(s_slots[i].state));
            break;
        default:
            snprintf(text, sizeof(text), "SLOT %d: %s", i,
                     meta_slot_list_word(s_slots[i].state));
            break;
        }
        lv_label_set_text(lbl, text);
    }
    rows_refresh(LIST_ITEMS, s_sel);
}

static void page_list_build(void)
{
    s_scr = ui_pixel_screen_create("meta-pass");
    add_row(s_scr, 0, 52, "");
    add_row(s_scr, 1, 96, "");
    add_row(s_scr, 2, 140, "");
    add_row(s_scr, 3, 184, "STORE DOWNLOAD");
    add_battery(s_scr);
    s_mascot = ui_pixel_mascot_create(s_scr, 101, 242);
    list_refresh();
    lv_screen_load(s_scr);
}

// ---------- 页面:槽位详情 ----------

static void page_detail_build(int slot)
{
    s_detail_slot = slot;
    meta_seq_reset(&s_egg_seq);   // 每次进入详情页重置彩蛋序列,避免残留干扰
    s_scr = ui_pixel_screen_create("SLOT");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 118, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);

    const meta_slot_info_t *s = &s_slots[slot];
    char text[220];
    if (s->state == META_SLOT_VALID) {
        snprintf(text, sizeof(text),
                 "%s%.16s\nver:  %.16s\nsize: %lu KB\nsha: %.16s...",
                 s->signed_fw ? "SIGNED\n" : "",
                 s->name, s->version, (unsigned long)(s->size / 1024), s->sha256_hex);
     } else {
         // r10:措辞单一事实源 meta_slots.c(empty=已擦除 / no firmware=有
         // 数据但非可引导镜像;两者都可覆盖安装,安装先擦除)。
         snprintf(text, sizeof(text), "%s", meta_slot_detail_word(s->state));
     }
     lv_label_set_text(s_info, text);

     add_row(s_scr, 0, 180, "BOOT");
     add_row(s_scr, 1, 224, "DELETE");
     add_row(s_scr, 2, 268, "BACK");
     rows_refresh(DETAIL_ITEMS, s_sel);
     lv_screen_load(s_scr);
}

// ---------- 页面:彩蛋(详情页隐藏序列 UP UP DOWN DOWN 快速四 CLICK 进入) ----------

static void page_egg_build(void)
{
    s_scr = ui_pixel_screen_create("EGG");
    // 可滚动面板:文本最长 3919B,远超一屏;UP/DOWN 按行滚动,OK 短按返回。
    s_egg_panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 180, UI_PAPER);
    lv_obj_set_scroll_dir(s_egg_panel, LV_DIR_VER);

    // 惰性读取+完整解析;static 缓冲 + set_text_static,避免 LVGL 堆内再复制 4KB。
    static char egg_buf[META_EGG_TEXT_LEN + 1];
    const meta_slot_info_t *s = &s_slots[s_detail_slot];
    const char *text;
    if (s->state != META_SLOT_VALID) {
        text = "No egg.";
    } else {
        const meta_egg_result_t r = meta_store_read_egg(s_detail_slot, s->size,
                                                        egg_buf, sizeof(egg_buf));
        text = (r == META_EGG_OK)     ? egg_buf
             : (r == META_EGG_ABSENT) ? "No egg."
                                      : "Egg data corrupted.";
    }

    lv_obj_t *lbl = lv_label_create(s_egg_panel);
    lv_obj_set_width(lbl, 196);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_INK), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text_static(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_screen_load(s_scr);
}

// ---------- 页面:二次确认 ----------

// 启动确认(未签名固件):警告文本 + BOOT/CANCEL 两个按钮行,UP/DOWN 选择,OK 短按确认。
// 默认停在 CANCEL:不可信固件不允许"一路 OK"误启动,保持原有的刻意操作门槛。
// OK LONG 沿用全局语义=返回详情页。
static void page_confirm_boot_build(void)
{
    s_scr = ui_pixel_screen_create("BOOT?");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 100, UI_PAPER);
    lv_obj_t *lbl = lv_label_create(panel);
    lv_obj_set_width(lbl, 196);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_INK), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 2, 2);
    // 未签名固件无法验明来源;签名固件在详情页 OK 直接启动,不会进入本页。
    lv_label_set_text(lbl, "Unsigned firmware!\nOnly boot if you trust\nthe source.");
    add_row(s_scr, 0, 164, "BOOT");
    add_row(s_scr, 1, 208, "CANCEL");
    s_sel = 1;                    // 默认 CANCEL(安全侧)
    rows_refresh(2, s_sel);
    ui_pixel_mascot_create(s_scr, 101, 256);
    lv_screen_load(s_scr);
}

// 删除确认:沿用 OK LONG 确认(防误删),OK 短按=取消。
static void page_confirm_del_build(void)
{
    s_scr = ui_pixel_screen_create("DELETE?");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 60, 216, 130, UI_PAPER);
    lv_obj_t *lbl = lv_label_create(panel);
    lv_obj_set_width(lbl, 196);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_INK), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_label_set_text(lbl, "Erase this slot?\nThis cannot be undone.\n\nOK LONG = delete\nOK click = cancel");
    ui_pixel_mascot_create(s_scr, 101, 242);
    lv_screen_load(s_scr);
}

// ---------- 商店页面(P0..P5) ----------

// 会话截止重置:任何商店页按键都会延长会话(网络全速开着,超时提示用户退出)。
static void store_touch(void)
{
    s_store_deadline = esp_timer_get_time() / 1000 + meta_store_session_timeout_ms();
}

// ---- 会话到期:不强制关闭,浮层提示用户决策(OK = 保留 / OK LONG = 退出) ----

static void store_show_timeout_prompt(void)
{
    if (s_timeout_lbl || !s_scr) return;
    // 与页面其它框同一像素立体风格:墨色错位阴影 + 纸底 + 4px 墨线边框
    // (ui_pixel_panel_create),浮层压在内容之上。ui_pixel_panel_create 会在
    // 父对象下先铺一块影子再放面板(两个兄弟对象),清理时要一起删。
    s_timeout_panel = ui_pixel_panel_create(s_scr, 32, 196, 176, 78, UI_PAPER);
    s_timeout_lbl = lv_label_create(s_timeout_panel);
    lv_obj_set_width(s_timeout_lbl, 148);
    lv_obj_set_style_text_font(s_timeout_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_timeout_lbl, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_align(s_timeout_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_timeout_lbl, "Session timeout.\nOK = continue\nLONG = exit store");
    lv_obj_center(s_timeout_lbl);
}

static void store_clear_timeout_prompt(void)
{
    // 只删面板会留下孤儿影子块(ui_pixel_panel_create 先铺影子再放面板,
    // 两个兄弟对象),按子索引把前一个兄弟一起拆;防御性判断防误删。
    if (s_timeout_panel) {
        lv_obj_t *parent = lv_obj_get_parent(s_timeout_panel);
        const int32_t idx = (int32_t)lv_obj_get_index(s_timeout_panel);
        lv_obj_t *shadow = (parent && idx > 0) ? lv_obj_get_child(parent, idx - 1) : NULL;
        if (shadow) lv_obj_delete(shadow);
        lv_obj_delete(s_timeout_panel);
        s_timeout_panel = NULL;
        s_timeout_lbl = NULL;
    }
}

// 定时器回调:商店各页共用。按当前页做对应轮询;会话到期出提示浮层等用户决策。
static void store_tick(lv_timer_t *t);

// 商店页内部迁移:只拆 LVGL 对象,不动网络栈(与 goto_page 的唯一差异)。
static void store_goto(page_t page);
static void page_store_id_build(void);
static void page_store_info_build(void);
static void page_store_slot_build(void);
static void page_store_dl_build(void);
static void page_store_cancel_build(void);
static void page_store_done_build(void);

// P2 详情内容是否已填充(防轮询定时器每拍重复填充/重复加行)。
static bool s_info_filled;

// P0 配网页:AP 态显示热点信息;已存凭证(CONNECTING/ONLINE)态显示当前 SSID
// 与 RESET WIFI 行 —— 选中并 OK 确认后擦凭证重开配网 AP(改 WiFi 入口)。
// ONLINE 停留 2s 后自动进 P1(给足"已连上 XX"的可见时间,再快也能看清 SSID)。
static int64_t  s_net_online_at;   // 进入 ONLINE 的时刻(自动进 P1 用;0=未在线)
// r10.1:改网意图 = P0 双击 UP(600ms 内);单击/其它键全部无动作(误按安全,
// 不会再把用户困在 P0)。检测器为纯逻辑(meta_store_prov,host 测试同一份)。
static meta_prov_upclick_t s_net_upclick;
// 连接失败原因的粘滞显示:失败 → ap_start 回到 AP_UP 只在一拍之间,ERROR 文案
// 会闪没;记下最近一次失败,配网页顶部展示 8s,让"提交后没反应"有因可读。
static char     s_net_last_fail[40];
static int64_t  s_net_fail_at;
// CONNECTING 已耗时(屏显 "Connecting... Ns"):任务侧上限 30s 连接 + 5s 时钟,
// 超过 60s 还在 CONNECTING = 任务挂死,屏上数字直接暴露,不用猜。
static int64_t  s_net_connecting_at;
static bool st_is_credentialed(void)
{
    meta_store_net_status_t st;
    meta_store_net_poll(&st);
    return st.state == SN_STATE_CONNECTING || st.state == SN_STATE_ONLINE
        || st.state == SN_STATE_ERROR;
}

static void page_store_net_build(void)
{
    s_scr = ui_pixel_screen_create("STORE");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 178, UI_PAPER);

    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);

    meta_store_net_status_t st;
    meta_store_net_poll(&st);
    char text[200];
    if (st.state == SN_STATE_AP_UP) {
        snprintf(text, sizeof(text),
                 "WiFi setup:\nhotspot: %s\n\nSetup page opens by\nitself. If not, open\nhttp://192.168.4.1\nPick network + password",
                 st.ssid);
        lv_label_set_text(s_info, text);
        s_net_online_at = 0;
        meta_prov_upclick_reset(&s_net_upclick);
    } else {
        snprintf(text, sizeof(text), "%s\n\nWiFi: %s", st.message,
                 st.sta_ssid[0] ? st.sta_ssid : "-");
        lv_label_set_text(s_info, text);
        // 已存凭证路径:r10.1 起改网入口 = 双击 UP(见按键分发);不再有
        // UP/DOWN 选中行,ERROR 态同样双击 UP 即可改网。
        meta_prov_upclick_reset(&s_net_upclick);
        // build 时的初始值仅作 tick 跳变检测的种子;真正的进入时刻由 tick 记录。
        s_net_online_at = (st.state == SN_STATE_ONLINE) ? esp_timer_get_time() / 1000 : 0;
    }

    s_status_line = lv_label_create(panel);
    lv_obj_set_width(s_status_line, 196);
    lv_obj_set_style_text_font(s_status_line, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_line, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_status_line, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    if (st.state != SN_STATE_AP_UP) {
        lv_label_set_text(s_status_line,
                          "double-UP = change WiFi\nhold OK = exit");
    } else {
        lv_label_set_text(s_status_line, "");
    }

    add_battery(s_scr);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// P1 屏上键盘(r9,r10 修正几何):15 键 4×4 —— 1 2 3 DEL / 4 5 6 CLR /
// 7 8 9 OK(纵跨两行) / ◀ 0 ▶。编辑模型在 meta_store_idedit.c(纯逻辑,
// host 测试同一份):插入光标、退格、显式 OK 提交、UP/DOWN 短按环移选中键、
// 长按换行、◀▶ 屏上键移光标。
// 几何契约(编译期锁死,几何回归在构建期直接失败):
//   - 面板底 52+84=136 < 键盘首行顶 142 → 键盘永不压进 ID 面板;
//   - OK 高 30+34=64,下缘 142+2*34+64=274 与 0 键下缘 142+3*34+30=274 对齐。
#define P1_PANEL_H       84
#define P1_KEY_H         30
#define P1_ROW_PITCH     34
#define P1_KEY_TOP       142
#define P1_OK_H          (P1_KEY_H + P1_ROW_PITCH)
_Static_assert(52 + P1_PANEL_H <= P1_KEY_TOP, "P1 panel must not overlap keypad");
_Static_assert(P1_KEY_TOP + 2 * P1_ROW_PITCH + P1_OK_H
               == P1_KEY_TOP + 3 * P1_ROW_PITCH + P1_KEY_H,
               "P1 OK bottom must align with 0-key bottom");
static mpd_idedit_t s_idedit;   // P1 编辑模型(纯逻辑,host 测试同一份;真实声明见键盘区)
static void id_key_refresh(void);

static void id_refresh_text(void)
{
    char line[40];
    mpd_idedit_render(&s_idedit, line, sizeof(line));
    lv_label_set_text(s_info, line);
    id_key_refresh();
}

static void page_store_id_build(void)
{
    s_scr = ui_pixel_screen_create("PLAY ID");
    // 面板 84 高(52..136):一行 ID + 两行提示,键盘首行 142 之上留 6px
    // (旧值 100 使键盘第一行压进面板 10px —— r10 修复,几何由上方
    // _Static_assert 锁死)。
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, P1_PANEL_H, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);

    s_status_line = lv_label_create(panel);
    lv_obj_set_width(s_status_line, 196);
    lv_obj_set_style_text_font(s_status_line, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_line, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_status_line, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    // 屏上提示只写看不见的手势:长按换行/长按退出。短按移动/按下在键盘上
    // 一眼自明,写了是废话(r10.3:上一版删反了,把必要的删了留下了废话)。
    lv_label_set_text(s_status_line, "hold UP/DOWN = row - hold OK = exit");

    // 屏上键盘 15 键(r9.1):4×4 —— 1 2 3 DEL / 4 5 6 CLR / 7 8 9 OK(纵跨
    // 两行) / ◀ 0 ▶。键 44x30,列距 50,行距 34;UP/DOWN 短按=环移选中键,
    // 长按=换行(同列无键停住);◀▶ 屏上键=移光标。
    static const char *const k_labels[MPD_KEY_COUNT] = {
        "1", "2", "3", "DEL",
        "4", "5", "6", "CLR",
        "7", "8", "9", "OK",
        "<", "0", ">",
    };
    for (int i = 0; i < MPD_KEY_COUNT; i++) {
        int row, col;
        mpd_key_row_col(i, &row, &col);
        const uint32_t base = (i == MPD_KEY_OK) ? UI_YELLOW
                            : (i == MPD_KEY_CLR || i == MPD_KEY_DEL
                               || i == MPD_KEY_LEFT || i == MPD_KEY_RIGHT) ? UI_MUTED
                            : UI_PAPER;
        // OK 纵跨 row2-row3:高 64,下缘与第三行数字键(0)下缘精确对齐
        // (等式由 _Static_assert 锁死;旧值 58 差 6px,r10 修复)。
        const int h = (i == MPD_KEY_OK) ? P1_OK_H : P1_KEY_H;
        s_keys[i] = ui_pixel_panel_create(s_scr, 12 + col * 50,
                                          P1_KEY_TOP + row * P1_ROW_PITCH,
                                          44, h, base);
        lv_obj_t *lbl = ui_pixel_label(s_keys[i], k_labels[i],
                                       &lv_font_montserrat_14, UI_INK);
        lv_obj_center(lbl);
    }
    mpd_idedit_init(&s_idedit);
    // r10.7:RETRY/BACK 回 P1 不丢输入 —— 有已确认过的 play_id 就预填,
    // 用户只需改数字或直接 OK;首次进入仍是空键盘。
    if (s_play_id > 0) {
        // uint32 最大 4294967295 = 10 位,12B 覆盖任意值(8B 会被 -Werror 的
        // format-truncation 拒绝 —— 门第一次发挥作用的正面案例)。
        char preset[12];
        snprintf(preset, sizeof(preset), "%lu", (unsigned long)s_play_id);
        (void)mpd_idedit_set_digits(&s_idedit, preset);
    }
    id_refresh_text();
    add_battery(s_scr);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// P1 键盘高亮刷新(r9):15 键;选中 = 亮青,OK 平时黄,CLR/DEL/◀▶ 平时灰,
// 数字纸白。
static lv_obj_t *k_key_obj(int idx)
{
    return (idx >= 0 && idx < MPD_KEY_COUNT) ? s_keys[idx] : NULL;
}

static void id_key_refresh(void)
{
    if (!s_scr) return;
    for (int i = 0; i < MPD_KEY_COUNT; i++) {
        const bool sel = (i == s_idedit.sel);
        uint32_t base = (i == MPD_KEY_OK) ? UI_YELLOW
                      : (i == MPD_KEY_CLR || i == MPD_KEY_DEL
                         || i == MPD_KEY_LEFT || i == MPD_KEY_RIGHT) ? UI_MUTED
                      : UI_PAPER;
        lv_obj_set_style_bg_color(k_key_obj(i),
                                  lv_color_hex(sel ? 0x35C4E8 : base), 0);
    }
}

// P1 → 组装编号并发 analyze,切到 P2 轮询。变长:至少 1 位。
static void id_commit(void)
{
    uint32_t id = 0;
    if (!mpd_idedit_value(&s_idedit, &id)) return;   // 空输入不提交
    s_play_id = id;
    if (meta_store_net_cmd_analyze(id) == ESP_OK) {
        store_goto(PAGE_STORE_INFO);
    } else {
        lv_label_set_text(s_status_line, "not online");
    }
}

// P2 详情页构建(analyze 进行中状态;结果经轮询填充)。
static void page_store_info_build(void)
{
    s_info_filled = false;
    s_scr = ui_pixel_screen_create("STORE");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 140, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_label_set_text(s_info, "Fetching info...");

    add_row(s_scr, 0, 196, "BACK");
    rows_refresh(1, 0);
    s_sel = 0;
    add_battery(s_scr);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// analyze 成功后填充详情内容。supported 时行 0 = CONFIRM、行 1 = BACK;
// 带 custom-partitions 警告时,详情里插一行警告文案(subtype 0x40 自定义数据
// 分区不随镜像进设备,运行时可能缺该存储,由用户决定是否继续)。
// r10.4:P2 是否渲染为"可重试失败页"(RETRY/BACK)。判定单一事实源:
//   - a == NULL:analyze 作业失败,结果未产出;
//   - 服务端失败码 unavailable(上游 5xx/回源失败,重试可能成功);
//   - 设备传输分类文案(含空格:"TLS failed (clock unsynced)." 等)。
// 其余 unsupported(not-found/too-large/wrong-chip/...)是终态,只给 BACK。
static bool store_info_is_retry_page(const meta_store_analysis_t *a)
{
    if (!a) return true;
    if (a->supported) return false;
    if (strcmp(a->reason, "unavailable") == 0) return true;
    return strchr(a->reason, ' ') != NULL;
}

// r10.7:RETRY 时的用户可见结果。cmd_analyze 在作业未 RUNNING 时入队成功;
// 入队失败仅提示,绝不清输入/翻页 —— 旧实现 store_goto(PAGE_STORE_ID) 把
// 三连按 RETRY 的用户扔回空键盘(输入清零,真机实测)。
static void store_info_retry(void)
{
    if (meta_store_net_cmd_analyze(s_play_id) != ESP_OK) {
        if (s_status_line) {
            lv_label_set_text(s_status_line, "Busy/Offline.\nHold OK = exit");
        }
    }
    // 入队成功:tick 在作业完成后重填本页(s_info_filled 已置 false)。
}

static void store_info_fill(const meta_store_analysis_t *a)
{
    s_info_filled = true;
    snprintf(s_store_warn_detail, sizeof(s_store_warn_detail), "%s", a->detail);
    char text[220];
    if (a->supported) {
        if (strcmp(a->reason, "custom-partitions") == 0) {
            snprintf(text, sizeof(text),
                     "%.24s\nsize: %lu KB\nmin slot: %d\nNOTE: custom '%.24s' part\nnot installed;\nsome features may lack it",
                     a->name, (unsigned long)(a->image_len / 1024), a->suggested_slot,
                     s_store_warn_detail);
        } else {
            snprintf(text, sizeof(text),
                     "%.24s\nsize: %lu KB\nmin slot: %d",
                     a->name, (unsigned long)(a->image_len / 1024), a->suggested_slot);
        }
    } else if (store_info_is_retry_page(a)) {
        // r10.4:可重试失败 —— reason + 层位 detail 都上屏,一次定位失败层。
        if (a->detail[0]) {
            snprintf(text, sizeof(text), "Failed: %.40s\n%.47s",
                     a->reason, a->detail);
        } else {
            snprintf(text, sizeof(text), "Failed: %.40s", a->reason);
        }
    } else {
        snprintf(text, sizeof(text),
                 "Not supported:\n%.24s", a->reason);
    }
    lv_label_set_text(s_info, text);

    if (store_info_is_retry_page(a)) {
        lv_obj_t *retry_lbl = lv_obj_get_child(s_rows[0], 0);
        lv_label_set_text(retry_lbl, "RETRY");
        add_row(s_scr, 1, 240, "BACK");
        rows_refresh(2, s_sel);
        return;
    }
    // 终态不支持:仅 BACK(行 0 标签直接写 BACK —— 旧版渲染 CONFIRM 却响应
    // BACK 行为,标签与行为不一致)。
    lv_obj_t *back_lbl = lv_obj_get_child(s_rows[0], 0);
    lv_label_set_text(back_lbl, "BACK");
    rows_refresh(1, s_sel);
}

// P3 槽位选择:行 = 3 槽,本地分区上限判 fit;仅 fit 行可确认。
static void page_store_slot_build(void)
{
    const meta_store_analysis_t *a = meta_store_net_analysis();
    s_scr = ui_pixel_screen_create("INSTALL");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 44, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
    char text[64];
    snprintf(text, sizeof(text), "%.20s\npick target slot:", a ? a->name : "?");
    lv_label_set_text(s_info, text);

    for (int i = 0; i < META_SLOT_COUNT; i++) {
        const esp_partition_t *part = meta_store_slot_partition(i);
        const uint32_t limit = part ? meta_sign_app_limit(part->size) : 0;
        s_slot_fit[i] = a && part && a->image_len <= limit;
        const bool occupied = s_slots[i].state == META_SLOT_VALID;
        char row[48];
        snprintf(row, sizeof(row), "SLOT %d %s%s", i,
                 s_slot_fit[i] ? "OK" : "TOO SMALL",
                 occupied ? " (will erase)" : "");
        add_row(s_scr, i, 100 + i * 44, row);
    }
    // 默认选中建议槽位(向上找第一个 fit,防建议槽位被本地占用标记干扰)。
    s_sel = a && a->suggested_slot >= 0 ? a->suggested_slot : 0;
    if (!s_slot_fit[s_sel]) {
        for (int i = 0; i < META_SLOT_COUNT; i++) {
            if (s_slot_fit[i]) { s_sel = i; break; }
        }
    }
    rows_refresh(META_SLOT_COUNT, s_sel);
    add_battery(s_scr);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// P4 下载进度页。
static void page_store_dl_build(void)
{
    s_scr = ui_pixel_screen_create("INSTALL");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 100, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_label_set_text(s_info, "Downloading...");

    s_status_line = lv_label_create(panel);
    lv_obj_set_width(s_status_line, 196);
    lv_obj_set_style_text_font(s_status_line, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_line, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_status_line, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_label_set_text(s_status_line, "0%");
    add_battery(s_scr);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// P5 完成提示。
static void page_store_done_build(void)
{
    s_scr = ui_pixel_screen_create("DONE");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 120, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
    const meta_store_analysis_t *a = meta_store_net_analysis();
    char text[160];
    snprintf(text, sizeof(text),
             "%.20s\ninstalled to slot %d.\n\nPower off & on to boot it.",
             a ? a->name : "Firmware", s_store_installed_slot);
    lv_label_set_text(s_info, text);
    add_row(s_scr, 0, 180, "BACK TO LIST");
    rows_refresh(1, 0);
    s_sel = 0;
    ui_pixel_mascot_create(s_scr, 101, 242);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// P4b 取消确认页:进入时下载仍在后台进行,三选一决策(取消/重试/继续等)。
static void page_store_cancel_build(void)
{
    s_scr = ui_pixel_screen_create("CANCEL?");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 76, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
    meta_store_api_progress_t p;
    meta_store_api_poll(&p);
    char text[96];
    snprintf(text, sizeof(text), "Download at %d%%.\nCancel it?",
             p.progress_pct > 0 ? p.progress_pct : 0);
    lv_label_set_text(s_info, text);

    add_row(s_scr, 0, 136, "CANCEL");   // 继续取消:作废半成品槽位
    add_row(s_scr, 1, 180, "RETRY");    // 重试:取消当前,终态时自动重新安装
    add_row(s_scr, 2, 224, "BACK");     // 返回:不作取消,继续等下载
    rows_refresh(3, 0);
    s_sel = 0;
    add_battery(s_scr);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// P4 失败页(与下载页共用骨架,信息换失败文案)。
static void page_store_dl_fail(const char *msg)
{
    if (s_info) lv_label_set_text(s_info, "Install failed:");
    if (s_status_line) lv_label_set_text(s_status_line, msg);
    // 行 0 复用为 BACK(对象尚不存在则新建)。
    add_row(s_scr, 0, 164, "BACK");
    rows_refresh(1, 0);
    s_sel = 0;
}

// 定时器回调:按页面轮询网络/作业状态并驱动页面迁移。
// 会话到期不自动关闭:置 s_store_expired 并出提示浮层,是否退出由用户按键决策
// (OK = 保留会话,OK LONG = 退出回列表;见 on_key)。作业进行中自动延续计时。
static void store_tick(lv_timer_t *t)
{
    (void)t;
    meta_store_net_job_t j;
    meta_store_net_job_poll(&j);
    const bool busy = (j.state == SN_JOB_RUNNING);
    if (busy) store_touch();

    if (s_store_expired) return;   // 等待用户决策:冻结自动迁移,网络保持原状

    if (!busy && esp_timer_get_time() / 1000 >= s_store_deadline) {
        s_store_expired = true;
        store_show_timeout_prompt();
        return;
    }

    switch (s_page) {
    case PAGE_STORE_NET: {
        meta_store_net_status_t st;
        meta_store_net_poll(&st);
        // 配网/连接是显式用户活动:每拍续期,300s 超时浮层不得在此触发 ——
        // 浮层会冻结面板在最后一帧(v13 真机"Syncing clock 卡 200s"的根因:
        // 凭证路径 busy=false,浮层照常触发,画面停在旧文字,任务其实早已
        // 结束)。超时语义保留给 P1 之后的闲置(输入页/进度页照旧问用户)。
        if (st.state == SN_STATE_AP_UP || st.state == SN_STATE_CONNECTING
            || st.state == SN_STATE_ERROR) {
            store_touch();
        }
        if (s_status_line) {
            char line[96];
            if (st.state == SN_STATE_AP_UP) {
                // r10.7:配网态每拍自动续期(BUG-07 教训),deadline 恒为满值 ——
                // 显示它等于显示一个不动的假倒计时(真机实测)。热点态没有
                // 超时压力,状态行显示真实状态句即可,不显示任何倒计时。
                snprintf(line, sizeof(line), "%s", st.message);
            } else {
                // CONNECTING/ERROR:同样每拍续期,倒计时无意义;错误态把
                // 原因句透传(比一个冻结的秒数有用得多)。
                snprintf(line, sizeof(line), "%s", st.message[0] ? st.message
                                        : "Connecting...");
            }
            lv_label_set_text(s_status_line, line);
        }
        if (st.state == SN_STATE_ONLINE) {
            // ONLINE 进入时刻在 tick 里做跳变检测,不用 build 时的旧值 ——
            // 页面在 CONNECTING 态构建时 online_at=0,任务随后真连上了,
            // 旧逻辑永远不翻页、面板冻在最后一帧(真机卡死主因)。首次见到
            // ONLINE:记时刻 + 刷面板(ONLINE 此前没有任何 s_info 分支)。
            if (!s_net_online_at) {
                s_net_online_at = esp_timer_get_time() / 1000;
                if (s_info) {
                    char text[200];
                    snprintf(text, sizeof(text), "Online.\n\nWiFi: %s",
                             st.sta_ssid[0] ? st.sta_ssid : "-");
                    lv_label_set_text(s_info, text);
                }
            }
            // 停留 2s 让用户看清连的哪个网,然后自动进 P1;r10.1:取消自动
            // 翻页的唯一方式是双击 UP(明确改网意图)——单击/误按不拦截。
            if (esp_timer_get_time() / 1000 - s_net_online_at >= 2000) {
                store_goto(PAGE_STORE_ID);
            }
        } else if (st.state == SN_STATE_AP_UP) {
            // AP_UP:配网信息。若 8s 内发生过连接失败,顶部加一行原因 ——
            // 失败回落 AP 只在一拍之间,不粘滞显示用户只会看到"没反应"。
            if (s_info) {
                char text[240];
                if (s_net_fail_at
                    && esp_timer_get_time() / 1000 - s_net_fail_at < 8000
                    && s_net_last_fail[0]) {
                    snprintf(text, sizeof(text),
                             "Last try: %s\n\nWiFi setup:\nhotspot: %s\n\nhttp://192.168.4.1\nPick network + password",
                             s_net_last_fail, st.ssid);
                } else {
                    snprintf(text, sizeof(text),
                             "WiFi setup:\nhotspot: %s\n\nSetup page opens by\nitself. If not, open\nhttp://192.168.4.1\nPick network + password",
                             st.ssid);
                }
                lv_label_set_text(s_info, text);
            }
        } else {
            // CONNECTING/ERROR:凭证已到手,实时刷新状态(此前这两态没有任何
            // 显示分支,屏钉死在配网文字上 —— 真机"提交后设备像死了"的直接
            // 原因)。CONNECTING 附带已耗时:任务侧上限 ~35s(30s 连接 + 5s 时钟),
            // 数字继续涨 = 任务挂死,一眼可判;归零/跳变 = 状态在走。ERROR 记入
            // 粘滞横幅,回落 AP 后仍可见 8s。
            if (st.state == SN_STATE_CONNECTING) {
                if (!s_net_connecting_at) {
                    s_net_connecting_at = esp_timer_get_time() / 1000;
                }
            } else {
                s_net_connecting_at = 0;
            }
            if (s_info) {
                char text[200];
                if (st.state == SN_STATE_CONNECTING) {
                    const int secs = (int)((esp_timer_get_time() / 1000
                                            - s_net_connecting_at) / 1000);
                    snprintf(text, sizeof(text), "%s (%ds)\n\nWiFi: %s",
                             st.message, secs,
                             st.sta_ssid[0] ? st.sta_ssid : "-");
                } else {
                    snprintf(text, sizeof(text), "%s\n\nWiFi: %s",
                             st.message, st.sta_ssid[0] ? st.sta_ssid : "-");
                }
                lv_label_set_text(s_info, text);
            }
            if (st.state == SN_STATE_ERROR && st.message[0]) {
                snprintf(s_net_last_fail, sizeof(s_net_last_fail), "%.39s", st.message);
                s_net_fail_at = esp_timer_get_time() / 1000;
            }
        }
        break;
    }

    case PAGE_STORE_INFO: {
        if (s_info_filled) break;
        meta_store_net_job_t j;
        meta_store_net_job_poll(&j);
        if (j.state == SN_JOB_RUNNING) break;   // 计时延续在 store_tick 顶部统一处理
        const meta_store_analysis_t *a = meta_store_net_analysis();
        if (a) {
            // analyze 成功(包括 P4 安装失败返回的本页:作业 DONE_FAIL 是安装残留,
            // 分析结果仍有效,直接展示,不被失败态覆盖)。
            store_info_fill(a);
        } else if (j.state == SN_JOB_DONE_FAIL && s_info) {
            // r10.4:analyze 失败页(a==NULL 的真路径)—— reason 码 + 层位
            // detail 都上屏,行 0 = RETRY(重新 analyze)、行 1 = BACK。
            s_info_filled = true;   // 失败也是终态:填充一次后停手
            char text[160];
            if (j.detail[0]) {
                snprintf(text, sizeof(text), "Failed: %.47s\n%.63s",
                         j.message, j.detail);
            } else {
                snprintf(text, sizeof(text), "Failed:\n%.47s", j.message);
            }
            lv_label_set_text(s_info, text);
            lv_obj_t *retry_lbl = lv_obj_get_child(s_rows[0], 0);
            lv_label_set_text(retry_lbl, "RETRY");
            add_row(s_scr, 1, 240, "BACK");
            s_sel = 0;
            rows_refresh(2, s_sel);
        }
        break;
    }

    case PAGE_STORE_DL: {
        meta_store_api_progress_t p;
        meta_store_api_poll(&p);
        if (s_status_line) {
            char line[64];
            if (p.active && !p.verify_phase) {
                snprintf(line, sizeof(line), "%d%%  %lu/%lu KB",
                         p.progress_pct,
                         (unsigned long)(p.received / 1024),
                         (unsigned long)(p.expected / 1024));
            } else {
                snprintf(line, sizeof(line), "%s", p.message);
            }
            lv_label_set_text(s_status_line, line);
        }
        meta_store_net_job_t j;
        meta_store_net_job_poll(&j);
        if (j.state == SN_JOB_DONE_OK) {
            s_store_retry = false;   // 取消请求晚于完成到达,重试标记不得残留
            store_goto(PAGE_STORE_DONE);
        } else if (j.state == SN_JOB_DONE_FAIL) {
            if (s_store_retry) {
                // P4b 选了 RETRY:当前下载已作废,立即重新安装同玩法同槽位。
                s_store_retry = false;
                if (meta_store_net_cmd_install(s_play_id, s_store_installed_slot)
                    == ESP_OK) {
                    break;   // 重新入队成功;进度快照由新 install 刷新
                }
                // 重新入队失败(不应发生:仍 ONLINE):落到下方失败页
            }
            if (!s_rows[0]) {
                page_store_dl_fail(j.message);   // 失败态只铺一次,行 0 = BACK
            }
        }
        break;
    }

    case PAGE_STORE_CANCEL: {
        // 决策期间下载可能已自行终结:完成 → P5;失败 → 回 P4 铺失败页/消费重试。
        meta_store_net_job_t j;
        meta_store_net_job_poll(&j);
        if (j.state == SN_JOB_DONE_OK) {
            s_store_retry = false;
            store_goto(PAGE_STORE_DONE);
        } else if (j.state == SN_JOB_DONE_FAIL) {
            store_goto(PAGE_STORE_DL);
        }
        break;
    }

    default:
        break;
    }
}

// ---------- 页面切换 ----------

static void goto_page(page_t page)
{
    page_teardown();
    s_page = page;
    s_sel = 0;
    switch (page) {
    case PAGE_LIST:     page_list_build();               break;
    case PAGE_DETAIL:   page_detail_build(s_detail_slot); break;
    case PAGE_CONFIRM_BOOT: page_confirm_boot_build();   break;
    case PAGE_CONFIRM_DEL:  page_confirm_del_build();    break;
    case PAGE_EGG:      page_egg_build();                break;
    case PAGE_STORE_NET: page_store_net_build();         break;
    case PAGE_STORE_ID:  page_store_id_build();          break;
    case PAGE_STORE_INFO: page_store_info_build();       break;
    case PAGE_STORE_SLOT: page_store_slot_build();       break;
    case PAGE_STORE_DL:  page_store_dl_build();          break;
    case PAGE_STORE_CANCEL: page_store_cancel_build();   break;
    case PAGE_STORE_DONE: page_store_done_build();       break;
    }
}

// 商店页内部迁移:拆 LVGL 对象但保留网络栈(goto_page 会停网络,中途翻页不能用)。
// 注意:商店页之间的迁移(含回 P0/P1/P2/P3)都必须走 store_goto;只有彻底离开
// 商店流程(P5 → 列表、任意商店页 OK LONG 回列表)才用 goto_page。
static void store_goto(page_t page)
{
    if (s_store_timer) {
        lv_timer_delete(s_store_timer);
        s_store_timer = NULL;
    }
    s_store_expired = false;   // 页面迁移只在未到期时发生;迁移即恢复普通计时
    if (s_scr) {
        lv_obj_delete(s_scr);   // 到期提示浮层随屏销毁
        s_scr = NULL;
        s_info = NULL;
        s_status_line = NULL;
        s_mascot = NULL;
        s_timeout_lbl = NULL;
        for (int i = 0; i < LIST_ITEMS; i++) s_rows[i] = NULL;
    }
    s_page = page;
    s_sel = 0;
    switch (page) {
    case PAGE_STORE_NET:  page_store_net_build();   break;
    case PAGE_STORE_ID:   page_store_id_build();    break;
    case PAGE_STORE_INFO: page_store_info_build();  break;
    case PAGE_STORE_SLOT: page_store_slot_build();  break;
    case PAGE_STORE_DL:   page_store_dl_build();    break;
    case PAGE_STORE_CANCEL: page_store_cancel_build(); break;
    case PAGE_STORE_DONE: page_store_done_build();  break;
    default: break;
    }
}

// ---------- 按键分发(运行于 button 组件任务,操作 LVGL 必须加锁) ----------

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    // 商店页面按键先处理会话到期决策:到期后冻结其他语义,只认
    // OK(继续当前操作,续期)/ OK LONG(退出回列表,teardown 停网络)。
    if (s_page >= PAGE_STORE_NET && s_page <= PAGE_STORE_DONE) {
        if (s_store_expired) {
            if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                s_store_expired = false;
                store_clear_timeout_prompt();
                store_touch();
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
                goto_page(PAGE_LIST);   // teardown 中 meta_store_net_stop()
            }
            bsp_lvgl_unlock();
            return;
        }
        store_touch();
    }

    switch (s_page) {
    case PAGE_LIST:
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP)   s_sel = (s_sel + LIST_ITEMS - 1) % LIST_ITEMS;
            if (btn == BSP_BTN_DOWN) s_sel = (s_sel + 1) % LIST_ITEMS;
            if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                list_refresh();
                ui_pixel_mascot_jump(s_mascot);
            } else if (btn == BSP_BTN_OK) {
                if (s_sel < META_SLOT_COUNT) {
                    const int slot = s_sel;
                    s_detail_slot = slot;
                    goto_page(PAGE_DETAIL);
                } else {
                    if (meta_store_net_init(s_slots) == ESP_OK
                        && meta_store_net_begin() == ESP_OK) {
                        goto_page(PAGE_STORE_NET);
                    }
                }
            }
        }
        break;

    case PAGE_DETAIL: {
        const meta_slot_info_t *s = &s_slots[s_detail_slot];

        // 隐藏彩蛋序列: 快速连按 UP UP DOWN DOWN,相邻两键间隔 <0.5s(meta_seq)。
        // 以 PRESS(按下瞬间)判定:每次物理按下必发、无延迟,快速连按可稳定凑齐四次。
        // 不能用 CLICK:SINGLE_CLICK 要等抬起后再过 180ms 判窗,窗内再按会被 button
        // 组件折叠成 DOUBLE/MULTIPLE_CLICK(iot_button.c PRESS_REPEAT_DOWN_CHECK),
        // 即第 2..4 次连按不再发 CLICK——快速连按永远凑不齐四个 CLICK(历史 bug,
        // 慢按则会被 PRESS 打断分支清进度,两条路都进不去)。LONG 仍打断序列。
        // 命中时吞掉第 4 次按下直接进彩蛋页;其后的 CLICK(抬起)落在彩蛋页等效
        // 一次滚动,属可接受副作用(与旧实现移动选中行同类)。
        if (ev == BSP_BTN_PRESS &&
            (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            const meta_seq_key_t k = (btn == BSP_BTN_UP) ? META_SEQ_KEY_UP
                                                         : META_SEQ_KEY_DOWN;
            if (meta_seq_feed(&s_egg_seq, k,
                              (uint32_t)(esp_timer_get_time() / 1000))) {
                goto_page(PAGE_EGG);   // 命中:吞掉第 4 个 CLICK,直接进彩蛋页
                break;
            }
        } else if (ev == BSP_BTN_LONG) {
            meta_seq_reset(&s_egg_seq);   // 长按(OK 长按=返回列表):打断序列
        }

        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP)   s_sel = (s_sel + DETAIL_ITEMS - 1) % DETAIL_ITEMS;
            if (btn == BSP_BTN_DOWN) s_sel = (s_sel + 1) % DETAIL_ITEMS;
            if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                rows_refresh(DETAIL_ITEMS, s_sel);
            } else if (btn == BSP_BTN_OK) {
                if (s_sel == 0 && meta_slot_bootable(s)) {          // BOOT
                    // 签名固件直接启动;未签名固件走 BOOT/CANCEL 菜单确认页
                    if (s->signed_fw) {
                        if (meta_store_boot_slot(s_detail_slot) == ESP_OK)
                            esp_restart();
                        goto_page(PAGE_DETAIL);
                    } else {
                        goto_page(PAGE_CONFIRM_BOOT);
                    }
                } else if (s_sel == 1 && s->state != META_SLOT_EMPTY) { // DELETE
                    goto_page(PAGE_CONFIRM_DEL);
                } else if (s_sel == 2) {                            // BACK
                    goto_page(PAGE_LIST);
                }
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            goto_page(PAGE_LIST);
        }
        break;
    }

    case PAGE_CONFIRM_BOOT: {
        // 隐藏彩蛋序列:与详情页一致,快速连按 UP UP DOWN DOWN(以 PRESS 判定,见上)。
        // 命中后其后的 CLICK(抬起)会切换一次选中项,属可接受副作用。
        if (ev == BSP_BTN_PRESS &&
            (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            const meta_seq_key_t k = (btn == BSP_BTN_UP) ? META_SEQ_KEY_UP
                                                         : META_SEQ_KEY_DOWN;
            if (meta_seq_feed(&s_egg_seq, k,
                              (uint32_t)(esp_timer_get_time() / 1000))) {
                goto_page(PAGE_EGG);   // 命中:吞掉第 4 个 CLICK,直接进彩蛋页
                break;
            }
        }
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                s_sel = (s_sel + 1) % 2;   // 两项菜单:UP/DOWN 均切换 BOOT/CANCEL
                rows_refresh(2, s_sel);
            } else if (btn == BSP_BTN_OK) {
                if (s_sel == 0) {          // BOOT:设置启动分区并重启,不返回
                    if (meta_store_boot_slot(s_detail_slot) == ESP_OK) {
                        esp_restart();
                    }
                }
                goto_page(PAGE_DETAIL);    // CANCEL 或设置失败(如分区损坏):回详情页
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            goto_page(PAGE_DETAIL);        // 全局语义:OK LONG=返回,即取消
        }
        break;
    }

    case PAGE_CONFIRM_DEL:
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {   // 取消
            goto_page(PAGE_DETAIL);
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            meta_store_erase_slot(s_detail_slot);
            meta_slot_clear(&s_slots[s_detail_slot]);
            goto_page(PAGE_LIST);
        }
        break;

    case PAGE_EGG:
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            goto_page(PAGE_DETAIL);   // 短按退出;LONG 有意忽略(序列末键 PRESS 之后仍会有 CLICK 到达)
        } else if ((btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) && ev == BSP_BTN_CLICK
                   && s_egg_panel) {
            const int step = lv_font_get_line_height(&lv_font_montserrat_14) * 4;
            lv_obj_scroll_by(s_egg_panel, 0, btn == BSP_BTN_UP ? step : -step, LV_ANIM_OFF);
        }
        break;

    case PAGE_STORE_NET:
        if (ev == BSP_BTN_CLICK) {
            if (st_is_credentialed() && btn == BSP_BTN_UP
                && meta_prov_upclick_feed(&s_net_upclick,
                                          esp_timer_get_time() / 1000)) {
                // 双击 UP(600ms 内)= 明确改网意图:擦凭证重开热点。
                // 单击/OK/DOWN 全部无动作 —— 误按不擦凭证、不拦自动进 P1。
                meta_prov_upclick_reset(&s_net_upclick);
                if (meta_store_net_reset_wifi() == ESP_OK) {
                    store_goto(PAGE_STORE_NET);   // reset 已 ap_start;重建显示热点
                }
            }
        }
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            goto_page(PAGE_LIST);   // 退出商店(商店统一出口,见 P1 同款)
        }
        break;

    case PAGE_STORE_ID:
        if (ev == BSP_BTN_CLICK) {
            bool changed = false;
            if (btn == BSP_BTN_UP) {
                mpd_idedit_move_ring(&s_idedit, -1);   // 短按 UP = 选中键环上逆行(高亮可见移动)
                changed = true;
            } else if (btn == BSP_BTN_DOWN) {
                mpd_idedit_move_ring(&s_idedit, +1);   // 短按 DOWN = 环上顺行
                changed = true;
            } else if (btn == BSP_BTN_OK) {
                changed = mpd_idedit_press(&s_idedit);
                if (s_idedit.commit_req) {
                    s_idedit.commit_req = false;
                    id_commit();
                    break;
                }
            }
            if (changed) id_refresh_text();
        } else if (ev == BSP_BTN_LONG
                   && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            // 长按 = 换行(UP 上一行 / DOWN 下一行,环绕,列尽量保持)。
            mpd_idedit_move_row(&s_idedit, btn == BSP_BTN_DOWN ? +1 : -1);
            id_refresh_text();
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            goto_page(PAGE_LIST);   // 退出商店(r10:不再是回 P0 —— P0 在线会自动
            // 回 P1,曾经死循环;商店唯一稳定出口统一为 OK LONG → 列表页)
        }
        break;

    case PAGE_STORE_INFO:
        if (ev == BSP_BTN_CLICK) {
            const meta_store_analysis_t *a = meta_store_net_analysis();
            // 三形态:正常页(CONFIRM/BACK)、可重试失败页(RETRY/BACK)、
            // 终态不支持页(仅 BACK)。判定单一事实源 = store_info_is_retry_page。
            const bool retry_page = store_info_is_retry_page(a);
            const int items = (a && a->supported) ? 2 : (retry_page ? 2 : 1);
            if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                s_sel = (s_sel + 1) % items;
                rows_refresh(items, s_sel);
            } else if (btn == BSP_BTN_OK) {
                if (retry_page && s_sel == 0) {
                    s_info_filled = false;   // RETRY:重新 analyze 同一玩法
                    store_info_retry();      // r10.7:失败只提示,不清输入不翻页
                } else if (a && a->supported && s_sel == 0) {
                    store_goto(PAGE_STORE_SLOT);
                } else {
                    store_goto(PAGE_STORE_ID);   // BACK = 重新输 ID
                }
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            store_goto(PAGE_STORE_ID);
        }
        break;

    case PAGE_STORE_SLOT:
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP)   s_sel = (s_sel + META_SLOT_COUNT - 1) % META_SLOT_COUNT;
            if (btn == BSP_BTN_DOWN) s_sel = (s_sel + 1) % META_SLOT_COUNT;
            if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                rows_refresh(META_SLOT_COUNT, s_sel);
            } else if (btn == BSP_BTN_OK && s_slot_fit[s_sel]) {
                if (meta_store_net_cmd_install(s_play_id, s_sel) == ESP_OK) {
                    s_store_installed_slot = s_sel;   // P5 展示用(store_goto 会清 s_sel)
                    s_store_retry = false;            // 新安装:清掉上次会话可能的残留
                    store_goto(PAGE_STORE_DL);
                }
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            store_goto(PAGE_STORE_INFO);
        }
        break;

    case PAGE_STORE_DL:
        // 下载中 OK LONG = 想取消 → 进确认页三选一(取消/重试/继续等);
        // 失败后会出现在行 0 的 BACK。
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK && s_rows[0]) {
            store_goto(PAGE_STORE_INFO);   // 失败态 BACK:回详情重试/换槽
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            meta_store_net_job_t j;
            meta_store_net_job_poll(&j);
            if (j.state == SN_JOB_RUNNING) {
                store_goto(PAGE_STORE_CANCEL);
            } else {
                store_goto(PAGE_STORE_INFO);
            }
        }
        break;

    case PAGE_STORE_CANCEL: {
        // 后台下载不停,此处只做决策:取消 / 重试 / 返回继续等。
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP)   s_sel = (s_sel + 2) % 3;
            if (btn == BSP_BTN_DOWN) s_sel = (s_sel + 1) % 3;
            if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                rows_refresh(3, s_sel);
            } else if (btn == BSP_BTN_OK) {
                if (s_sel == 0) {              // CANCEL:继续取消,作废半成品槽位
                    meta_store_api_request_cancel();
                    store_goto(PAGE_STORE_DL);
                } else if (s_sel == 1) {       // RETRY:取消当前,终态时自动重装
                    s_store_retry = true;
                    meta_store_api_request_cancel();
                    store_goto(PAGE_STORE_DL);
                } else {                       // BACK:不取消,继续等下载
                    store_goto(PAGE_STORE_DL);
                }
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            store_goto(PAGE_STORE_DL);   // 全局返回语义 = 不作取消,继续等
        }
        break;
    }

    case PAGE_STORE_DONE:
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            goto_page(PAGE_LIST);
        }
        break;
    }

    bsp_lvgl_unlock();
}

// ---------- 入口 ----------

void app_main(void)
{
    ESP_LOGI(TAG, "meta-pass launcher 启动");

    bsp_i2c_init();
    bsp_i2c_scan();

    // 显示是 UI 硬依赖,失败直接退出(沿用基线纪律)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,启动器无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 按键/电池为软依赖:失败只影响对应能力,不阻塞启动器。
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,设备将无法操作");
    }
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电池计不在位,电量显示降级");
    }

    // 单次会话模型:每次开机清空 otadata,保证下次上电 bootloader 默认引导
    // factory 列表页(签名子固件也不跨重启常驻)。
    // 边界:若设备正被旧版 hook 写入 VALID 的常驻子固件引导,本代码不会执行
    // (启动器未被引导);那种设备用子固件返回钩子(OK 长按)或重装解锁。
    const esp_err_t mv = meta_store_mark_factory_valid();
    if (mv != ESP_OK) {
        ESP_LOGW(TAG, "otadata 清除失败(%s)", esp_err_to_name(mv));
    }

    meta_store_scan(s_slots);
    // 软失败不阻塞启动器;进入 STORE 时(button 回调)会以 s_slots 重试一次。
    meta_store_net_init(s_slots);

    if (bsp_lvgl_lock(1000)) {
        page_list_build();
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "就绪:slot0=%d slot1=%d slot2=%d", s_slots[0].state, s_slots[1].state, s_slots[2].state);
}
