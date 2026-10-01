// main/main.c —— meta-pass 多固件启动器:槽位管理 + LAN 手机辅助安装 + 引导。
//
// 设计文档(单一权威来源):docs/assets/meta-pass-design.md;LAN 安装通道见
// docs/assets/lan-pair-install-design.md 与 main/meta_store_install.h。
//
// 商店流程(feat/mota 净切,手机承担浏览/下载/剥离,设备只确认与写入):
//   P0 配网   SoftAP 表单收 WiFi 凭证(或复用已存凭证直连)→ STA → ONLINE
//   P1 扫码   起本地 install HTTP 服务,上屏 QR(http://ip/#s=token)+ 配对码
//   P2 确认   手机 prepare 后展示名称/体积/原因;CONFIRM 进槽位页 / BACK 拒绝
//   P3 选槽   三个槽位行,本地分区上限判 fit;OK 物理确认(上传前提,§8)
//   P4 上传   轮询 install 快照:session 开启 → uploading → done/failed
//   P4b 取消  上传中 OK LONG 进确认页:CANCEL=作废半成品 / BACK=继续等
//   P5 提示   安装完成,提示断电重启选择子固件
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
#include "meta_store_net.h"
#include "meta_store_install.h"
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
    PAGE_STORE_QR,     // P1 商店:QR + 配对码(本地 install 服务已启动)
    PAGE_STORE_INFO,   // P2 商店:手机 prepare 的 install offer 确认
    PAGE_STORE_SLOT,   // P3 商店:目标槽位选择(物理确认)
    PAGE_STORE_DL,     // P4 商店:LAN 分块上传进度
    PAGE_STORE_CANCEL, // P4b 商店:取消上传确认(CANCEL/BACK,会话仍在)
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

// ---- 商店会话状态(UI 上下文;安装会话状态机在 meta_store_install.c) ----
static meta_install_manifest_t s_offer;          // P2/P3 展示中的 install offer 快照
static bool s_offer_valid;                       // s_offer 是否有效(页面重建复用)
static bool s_qr_service_failed;                 // P1 起本地 install 服务失败的粘滞提示
// 生命周期纪律:凡"页面重建后仍需有效"的状态由 store_goto/各 build 显式维护,
// 不能依赖 LVGL 对象存活;offer 快照在 P1→P2 迁移时填充,回 P1 即丢弃。
static bool     s_slot_fit[META_SLOT_COUNT];     // P3 各槽位 fit 标记(本地分区上限)
static int      s_store_installed_slot;          // P3 确认的目标槽位(P5 展示用;store_goto 会清 s_sel)
static bool     s_store_expired;                 // 会话已到期,等待用户决策(冻结自动迁移)
static lv_obj_t *s_timeout_panel;                // 到期提示浮层本体(ui_pixel_panel 立体框)
static lv_obj_t *s_timeout_lbl;                  // 浮层标签(s_timeout_panel 子对象)

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
        // r10.10:P3 不适配槽位行置灰(ui_pixel_set_selected enabled=false);
        // 其他页全部可用(enabled=true)。fit 表只在 P3 有效,页值缺省 true。
        const bool enabled = (s_page != PAGE_STORE_SLOT) || s_slot_fit[i];
        ui_pixel_set_selected(s_rows[i], i == sel, enabled);
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
        meta_store_net_stop();     // 完整释放 httpd/wifi(资源纪律见 meta_store_net.c)
        meta_install_net_stop();   // 本地 install 服务与 token 一并作废(§8)
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
static void page_store_qr_build(void);
static void page_store_info_build(void);
static void page_store_slot_build(void);
static void page_store_dl_build(void);
static void page_store_cancel_build(void);
static void page_store_done_build(void);

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

// P1 扫码页(§6.1):上屏 install URL 的 QR(http://ip/#s=token,token 在
// fragment 不进初始 HTTP 请求)+ 文本兜底(IP + 配对码)。本页无行项:
// UP/DOWN 无动作,OK LONG = 退出商店;闲置超时由会话浮层提示。
// 屏宽 240:QR 120px 居中(x=60),配对码面板垫在下方。
static void page_store_qr_build(void)
{
    s_offer_valid = false;   // 回到扫码页即丢弃本地 offer 快照(重 prepare 再取)
    s_scr = ui_pixel_screen_create("SCAN ME");

    if (s_qr_service_failed) {
        // 本地 install 服务启动失败:QR 无意义(手机连不上),文字交代后果。
        lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 92, 216, 100, UI_PAPER);
        s_info = lv_label_create(panel);
        lv_obj_set_width(s_info, 196);
        lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
        lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
        lv_label_set_text(s_info, "LAN install service\nfailed to start.\nHold OK = exit");
        add_battery(s_scr);
        store_touch();
        s_store_timer = lv_timer_create(store_tick, 250, NULL);
        lv_screen_load(s_scr);
        return;
    }

    const char *token_hex = NULL;
    const char *pair = NULL;
    char url[256];
    meta_install_qr_info(&token_hex, &pair, url);

    if (token_hex) {
        lv_obj_t *qr = lv_qrcode_create(s_scr);
        lv_qrcode_set_size(qr, 120);
        lv_qrcode_set_dark_color(qr, lv_color_hex(UI_INK));
        lv_qrcode_set_light_color(qr, lv_color_hex(UI_PAPER));
        lv_qrcode_set_quiet_zone(qr, true);
        // 诊断(真机 bring-up):lv_qrcode_update 此前静默失败时页面只剩文字、
        // 串口无任何线索 —— 两个分支都必须留痕。
        ESP_LOGI(TAG, "qr: encoding %u-byte url", (unsigned)strlen(url));
        const lv_result_t qr_rc = lv_qrcode_update(qr, url, (uint32_t)strlen(url));
        if (qr_rc != LV_RESULT_OK) {
            ESP_LOGE(TAG, "qr update failed: rc=%d url=%u bytes (token=%s)",
                     (int)qr_rc, (unsigned)strlen(url), token_hex);
            lv_obj_delete(qr);
        } else {
            lv_obj_set_pos(qr, 60, 46);
            ESP_LOGI(TAG, "qr rendered 120px at (60,46)");
            // 回读画布像素给 QR 不可见定论:中心_finder 区(30,30)应深、
            // quiet zone(2,2)应浅。两样本都对而屏上无物 = 显示/刷新链路
            // 问题;样本不对 = 编码/调色板问题。lv_canvas_get_px 为本地坐标,
            // LVGL 9.5 返回 lv_color32_t(非 lv_color_t)。
            const lv_color32_t px_dark = lv_canvas_get_px(qr, 30, 30);
            const lv_color32_t px_light = lv_canvas_get_px(qr, 2, 2);
            ESP_LOGI(TAG, "qr px(30,30)=%02x%02x%02x px(2,2)=%02x%02x%02x (ink=%06x paper=%06x)",
                     px_dark.red, px_dark.green, px_dark.blue,
                     px_light.red, px_light.green, px_light.blue,
                     UI_INK, UI_PAPER);
        }
    }

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 172, 216, 88, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
    {
        char ip_txt[20] = "0.0.0.0";
        const uint32_t ip = meta_install_lan_ip();
        if (ip != 0) {
            const uint8_t *b = (const uint8_t *)&ip;   // 网络字节序,内存序即 a.b.c.d
            snprintf(ip_txt, sizeof(ip_txt), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }
        char text[96];
        snprintf(text, sizeof(text), "scan QR, or open:\nhttp://%s\npair code: %s",
                 ip_txt, (pair && !token_hex) ? "-" : (pair ? pair : "-"));
        lv_label_set_text(s_info, text);
    }

    s_status_line = lv_label_create(panel);
    lv_obj_set_width(s_status_line, 196);
    lv_obj_set_style_text_font(s_status_line, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_line, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_status_line, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_label_set_text(s_status_line, token_hex ? "waiting for phone..." : "token error");

    add_battery(s_scr);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// P2 offer 确认页(§6.4):手机 prepare 后展示名称/体积/原因;行 0 = CONFIRM
// (进槽位页),行 1 = BACK(设备侧拒绝,手机可重新 prepare)。
static void page_store_info_build(void)
{
    s_scr = ui_pixel_screen_create("INSTALL");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 138, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
    char text[224];
    if (!s_offer_valid) {
        // 防御路径:P2 只应从 P1 携有效快照进入;置空快照时只给 BACK 出口。
        snprintf(text, sizeof(text), "No pending offer.");
    } else if (s_offer.reason[0] && strcmp(s_offer.reason, "ok") != 0) {
        // 手机侧判断非 ok(如 custom-partitions):原因句透传上屏。
        snprintf(text, sizeof(text),
                 "Install: %.24s\n%lu KB\nNOTE: %.40s",
                 s_offer.name, (unsigned long)(s_offer.image_len / 1024), s_offer.reason);
    } else {
        snprintf(text, sizeof(text), "Install: %.24s\n%lu KB",
                 s_offer.name, (unsigned long)(s_offer.image_len / 1024));
    }
    lv_label_set_text(s_info, text);

    add_row(s_scr, 0, 196, "CONFIRM");
    add_row(s_scr, 1, 240, "BACK");
    s_sel = 0;
    rows_refresh(2, s_sel);
    add_battery(s_scr);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// P3 槽位选择(§6.4):行 = 3 槽,本地分区上限判 fit;仅 fit 行可确认。
// OK = 物理确认(上传前提,§8);确认后手机才被允许开上传 session。
static void page_store_slot_build(void)
{
    s_scr = ui_pixel_screen_create("SLOT?");
    // 信息窗与按钮行拉开层次(同 r10.10:正文弱化,避免像两个乱码按钮)。
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 88, UI_MUTED);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
    char text[64];
    snprintf(text, sizeof(text), "%.20s\n%lu KB -> pick slot",
             s_offer_valid ? s_offer.name : "?",
             s_offer_valid ? (unsigned long)(s_offer.image_len / 1024) : 0);
    lv_label_set_text(s_info, text);

    for (int i = 0; i < META_SLOT_COUNT; i++) {
        const esp_partition_t *part = meta_store_slot_partition(i);
        const uint32_t limit = part ? meta_sign_app_limit(part->size) : 0;
        s_slot_fit[i] = s_offer_valid && part && s_offer.image_len <= limit;
        const bool occupied = s_slots[i].state == META_SLOT_VALID;
        // 占用槽位标 "erase"(esp_ota_begin 先擦除,可覆盖);不适配槽位
        // 短写 "too small" 防超宽截断(同 r10.10)。
        char row[24];
        if (s_slot_fit[i]) {
            snprintf(row, sizeof(row), "SLOT %d%s", i, occupied ? " erase" : "");
        } else {
            snprintf(row, sizeof(row), "SLOT %d too small", i);
        }
        add_row(s_scr, i, 100 + i * 44, row);
    }
    // 默认选中建议槽位(模型层:suggestedSlot 本地 fit 才用,否则首个可用)。
    const int8_t dflt = meta_install_default_slot_from_manifest(&s_offer);
    s_sel = (dflt >= 0) ? dflt : 0;
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

// P4 上传进度页:轮询 install 快照(手机开 session → 分块写入 → 终态)。
static void page_store_dl_build(void)
{
    s_scr = ui_pixel_screen_create("UPLOAD");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 100, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
    char text[64];
    snprintf(text, sizeof(text), "Uploading...\n%.20s",
             s_offer_valid ? s_offer.name : "");
    lv_label_set_text(s_info, text);

    s_status_line = lv_label_create(panel);
    lv_obj_set_width(s_status_line, 196);
    lv_obj_set_style_text_font(s_status_line, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_line, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_status_line, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_label_set_text(s_status_line, "waiting session...");
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
    // 完成名取自 install 快照(finalize 成功后保留 name/slot 供本页展示)。
    meta_install_session_status_t st;
    meta_install_session_poll(&st);
    char text[160];
    snprintf(text, sizeof(text),
             "%.20s\ninstalled to slot %d.\n\nPower off & on to boot it.",
             st.name[0] ? st.name : "Firmware", s_store_installed_slot);
    lv_label_set_text(s_info, text);
    add_row(s_scr, 0, 180, "BACK TO LIST");
    rows_refresh(1, 0);
    s_sel = 0;
    ui_pixel_mascot_create(s_scr, 101, 242);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// P4b 取消确认页:进入时上传仍在后台进行,二选一决策(取消/继续等)。
static void page_store_cancel_build(void)
{
    s_scr = ui_pixel_screen_create("CANCEL?");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 76, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_label_set_text(s_info, "Cancel upload?\nWritten data is erased.");

    add_row(s_scr, 0, 136, "CANCEL");   // 取消:作废半成品(flash 动过即槽位 INVALID)
    add_row(s_scr, 1, 180, "BACK");     // 返回:不作取消,继续等上传
    s_sel = 1;                          // 默认 BACK(安全侧,同启动确认页纪律)
    rows_refresh(2, s_sel);
    add_battery(s_scr);
    store_touch();
    s_store_timer = lv_timer_create(store_tick, 250, NULL);
    lv_screen_load(s_scr);
}

// 定时器回调:按页面轮询网络/安装会话状态并驱动页面迁移。
// 会话到期不自动关闭:置 s_store_expired 并出提示浮层,是否退出由用户按键决策
// (OK = 保留会话,OK LONG = 退出回列表;见 on_key)。上传进行中自动延续计时。
static void store_tick(lv_timer_t *t)
{
    (void)t;
    meta_install_session_status_t ist;
    meta_install_session_poll(&ist);
    const char *ist_state = ist.state ? ist.state : "";
    // 上传活动续期(审计 M6):只有 chunk 活动才算"传输中";uploading 是闩锁
    // 状态,手机消失后它永远为真。停滞超阈值即停止续期,由下面的死线检查
    // 出超时浮层 —— 设计 §6.5"中断保持会话到超时或显式取消"的超时半边。
    const bool uploading = (strcmp(ist_state, "uploading") == 0);
    const bool stalled = uploading &&
        ist.upload_idle_ms >= (int64_t)META_INSTALL_UPLOAD_STALL_MS;
    const bool busy = uploading && !stalled;
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
        // 结束)。超时语义保留给 P1 之后的闲置(扫码页/进度页照旧问用户)。
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
            // 停留 2s 让用户看清连的哪个网,然后进 P1(§6.1 步骤 3/4):先起
            // 本地 install 服务,再签发 token/配对码;起服务失败粘滞上屏
            // (无服务时 QR 无意义,文字交代后果)。
            if (esp_timer_get_time() / 1000 - s_net_online_at >= 2000) {
                s_qr_service_failed = (meta_install_net_start() != ESP_OK);
                if (!s_qr_service_failed) {
                    (void)meta_install_token_start();
                }
                store_goto(PAGE_STORE_QR);
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
            // 原因)。CONNECTING 附带已耗时:任务侧上限 ~30s,数字继续涨 =
            // 任务挂死,一眼可判;归零/跳变 = 状态在走。ERROR 记入粘滞横幅,
            // 回落 AP 后仍可见 8s。
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

    case PAGE_STORE_QR: {
        // 手机 prepare 到货:取一次快照进 P2。confirmed 时不迁(防御:
        // 正常路径 confirmed 只发生在 P3 之后)。
        if (ist.offer_ready && !ist.confirmed && !s_offer_valid
            && meta_install_offer_copy(&s_offer)) {
            s_offer_valid = true;
            store_goto(PAGE_STORE_INFO);
            break;
        }
        if (s_status_line) {
            lv_label_set_text(s_status_line,
                              ist.message[0] ? ist.message : "waiting for phone...");
        }
        break;
    }

    case PAGE_STORE_INFO:
        // 纯决策页:offer 在进入时已填充,无轮询迁移。
        break;

    case PAGE_STORE_SLOT:
        // 同上:选槽是纯决策;确认动作在 on_key。
        break;

    case PAGE_STORE_DL: {
        // 终态迁移:done → P5;cancelled(手机侧)→ 回扫码页等新 offer;
        // failed 停留本页,状态行给出原因,OK 短按回扫码页(见 on_key)。
        if (strcmp(ist_state, "done") == 0) {
            store_goto(PAGE_STORE_DONE);
            break;
        }
        if (strcmp(ist_state, "cancelled") == 0) {
            store_goto(PAGE_STORE_QR);
            break;
        }
        if (s_status_line) {
            char line[80];
            if (strcmp(ist_state, "failed") == 0) {
                snprintf(line, sizeof(line), "failed: %.30s", ist.message);
            } else if (ist.session_opened) {
                snprintf(line, sizeof(line), "%u / %u KB",
                         (unsigned)(ist.offset / 1024),
                         (unsigned)(ist.expected / 1024));
            } else {
                snprintf(line, sizeof(line), "waiting session...");
            }
            lv_label_set_text(s_status_line, line);
        }
        break;
    }

    case PAGE_STORE_CANCEL: {
        // 决策期间上传可能已自行终结:完成 → P5;失败/取消 → 回 P4 呈现事实。
        if (strcmp(ist_state, "done") == 0) {
            store_goto(PAGE_STORE_DONE);
        } else if (strcmp(ist_state, "failed") == 0
                   || strcmp(ist_state, "cancelled") == 0) {
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
    case PAGE_STORE_QR:  page_store_qr_build();          break;
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
    case PAGE_STORE_QR:   page_store_qr_build();    break;
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
                    if (meta_store_net_init() == ESP_OK
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
            goto_page(PAGE_LIST);   // 退出商店(商店统一出口)
        }
        break;

    case PAGE_STORE_QR:
        // 扫码页无行项:UP/DOWN/OK 短按无动作;OK LONG = 退出商店。
        // 闲置提示交给会话超时浮层(OK=继续等,LONG=退出)。
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            goto_page(PAGE_LIST);
        }
        break;

    case PAGE_STORE_INFO:
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                s_sel = (s_sel + 1) % 2;   // 两行:CONFIRM / BACK
                rows_refresh(2, s_sel);
            } else if (btn == BSP_BTN_OK) {
                if (s_sel == 0) {
                    store_goto(PAGE_STORE_SLOT);   // 进槽位选择
                } else {
                    // 设备侧拒绝 offer(§6.4):清 offer 回 pairing,
                    // 手机可重新 prepare(同 token)。
                    meta_install_offer_reject();
                    store_goto(PAGE_STORE_QR);
                }
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            meta_install_offer_reject();   // 全局返回语义 = 拒绝并回扫码页
            store_goto(PAGE_STORE_QR);
        }
        break;

    case PAGE_STORE_SLOT:
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP)   s_sel = (s_sel + META_SLOT_COUNT - 1) % META_SLOT_COUNT;
            if (btn == BSP_BTN_DOWN) s_sel = (s_sel + 1) % META_SLOT_COUNT;
            if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                rows_refresh(META_SLOT_COUNT, s_sel);
            } else if (btn == BSP_BTN_OK && s_slot_fit[s_sel]) {
                // 物理确认(§6.4/§8):只有这一步能解锁手机侧上传 session。
                if (meta_install_confirm_slot((int8_t)s_sel) == ESP_OK) {
                    s_store_installed_slot = s_sel;   // P5 展示用(store_goto 会清 s_sel)
                    store_goto(PAGE_STORE_DL);
                } else if (s_status_line) {
                    lv_label_set_text(s_status_line, "cannot confirm slot");
                }
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            store_goto(PAGE_STORE_INFO);   // 返回确认页(offer 仍在途)
        }
        break;

    case PAGE_STORE_DL:
        // 失败态 OK 短按 = 回扫码页(手机可重新 prepare);
        // OK LONG = 想取消 → 决策页(CANCEL/BACK)。
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            meta_install_session_status_t st;
            meta_install_session_poll(&st);
            if (st.state && strcmp(st.state, "failed") == 0) {
                store_goto(PAGE_STORE_QR);
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            meta_install_session_status_t st;
            meta_install_session_poll(&st);
            if (!st.state || strcmp(st.state, "done") != 0) {
                store_goto(PAGE_STORE_CANCEL);
            }
        }
        break;

    case PAGE_STORE_CANCEL:
        // 后台上传不停,此处只做决策:取消 / 返回继续等。
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                s_sel = (s_sel + 1) % 2;   // 两行:CANCEL / BACK
                rows_refresh(2, s_sel);
            } else if (btn == BSP_BTN_OK) {
                if (s_sel == 0) {
                    meta_install_cancel();   // 作废半成品(flash 动过即槽位 INVALID)
                    store_goto(PAGE_STORE_QR);
                } else {
                    store_goto(PAGE_STORE_DL);   // BACK:不作取消,继续等
                }
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            store_goto(PAGE_STORE_DL);   // 全局返回语义 = 不作取消
        }
        break;

    case PAGE_STORE_DONE:
        if (btn == BSP_BTN_OK) {   // 短按/长按均回列表(离店 teardown 停网络)
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
    // feat/mota 净切:net_init 只备好配网/上线(无 WAN 作业链);LAN install
    // 通道在此登记槽位注册表(安装成功回写),生命周期覆盖整个启动器。
    meta_store_net_init();
    meta_install_net_init(s_slots);

    if (bsp_lvgl_lock(1000)) {
        page_list_build();
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "就绪:slot0=%d slot1=%d slot2=%d", s_slots[0].state, s_slots[1].state, s_slots[2].state);
}
