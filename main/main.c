// main/main.c —— meta-pass 多固件启动器:槽位管理 + 导入 + 引导。
//
// 设计文档(单一权威来源):docs/assets/meta-pass-design.md
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
#include "meta_net.h"
#include "meta_seq.h"
#include "meta_slots.h"
#include "meta_store.h"
#include "ui_pixel.h"

static const char *TAG = "meta-pass";

// 页面枚举:每个页面独立 build/teardown(沿用基线 demo 的建删屏纪律)。
typedef enum {
    PAGE_LIST = 0,     // 槽位列表 + Import 入口
    PAGE_DETAIL,       // 槽位详情:Boot / Delete / Back
    PAGE_CONFIRM_BOOT, // 未签名固件启动警告
    PAGE_CONFIRM_DEL,  // 删除确认
    PAGE_IMPORT,       // SoftAP 导入页
    PAGE_EGG,          // 彩蛋页:隐藏序列进入,滚动查看 MAEG 文本
} page_t;

#define LIST_ITEMS   4                   // Slot 0 / Slot 1 / Slot 2 / Import
#define DETAIL_ITEMS 3                   // Boot / Delete / Back
#define IMPORT_TIMEOUT_MS (5 * 60 * 1000)  // 导入会话无操作自动关闭(设计文档 §6)

static meta_slot_info_t s_slots[META_SLOT_COUNT];  // 槽位注册表(meta_net 上传成功也回写它)

static page_t    s_page = PAGE_LIST;
static int       s_sel;              // 当前页选中行
static int       s_detail_slot;      // 详情/确认页操作的槽位
static int64_t   s_import_deadline;  // 导入页自动关闭时刻(ms,esp_timer 时基)
static lv_timer_t *s_import_timer;   // 导入页轮询定时器(离开页面前必须删)

static lv_obj_t *s_scr;              // 当前页 screen;同一时间只有一个
static lv_obj_t *s_rows[LIST_ITEMS]; // 可选中行面板(数量按页面上限分配)
static lv_obj_t *s_info;             // 详情/导入页的多行文本
static lv_obj_t *s_status_line;      // 导入页状态行
static lv_obj_t *s_egg_panel;        // 彩蛋页可滚动面板(teardown 时随屏销毁)
static lv_obj_t *s_mascot;
static meta_seq_state_t s_egg_seq;   // 详情页隐藏序列 UP UP DOWN DOWN(四 CLICK)的匹配状态

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
    if (s_import_timer) {
        lv_timer_delete(s_import_timer);
        s_import_timer = NULL;
    }
    if (s_page == PAGE_IMPORT) {
        meta_net_stop();   // 完整释放 httpd/wifi/netif(资源纪律见 meta_net.c)
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_info = NULL;
        s_status_line = NULL;
        s_egg_panel = NULL;
        s_mascot = NULL;
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
            snprintf(text, sizeof(text), "SLOT %d: (invalid)", i);
            break;
        default:
            snprintf(text, sizeof(text), "SLOT %d: (empty)", i);
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
    add_row(s_scr, 3, 184, "IMPORT FIRMWARE");
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
         snprintf(text, sizeof(text), "%s", s->state == META_SLOT_EMPTY
                  ? "(empty)\nImport firmware first." : "(invalid)\nDelete it and re-import.");
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

// ---------- 页面:导入(SoftAP + 网页上传) ----------

// LVGL 定时器上下文运行,可直接操作对象;网络状态经 poll 快照读取。
static void import_tick(lv_timer_t *t)
{
    (void)t;
    meta_net_status_t st;
    meta_net_poll(&st);
    if (s_status_line) {
        char line[96];
        const int left = (int)((s_import_deadline - esp_timer_get_time() / 1000) / 1000);
        snprintf(line, sizeof(line), "%s\nclosing in %ds", st.message, left > 0 ? left : 0);
        lv_label_set_text(s_status_line, line);
    }
    if (esp_timer_get_time() / 1000 >= s_import_deadline) {
        // 超时自动关闭:回到列表页(teardown 里会完整停掉网络栈)。
        page_teardown();
        s_page = PAGE_LIST;
        s_sel = 0;
        page_list_build();
    }
}

static void page_import_build(void)
{
    esp_err_t err = meta_net_start(s_slots);
    s_scr = ui_pixel_screen_create("IMPORT");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 178, UI_PAPER);

    s_info = lv_label_create(panel);
    lv_obj_set_width(s_info, 196);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 2, 2);

    meta_net_status_t st;
    meta_net_poll(&st);
    char text[200];
    if (err == ESP_OK) {
        snprintf(text, sizeof(text),
                 "SSID: %s\npass: %s\ncode: %s\n\nopen http://192.168.4.1\nenter code, pick slot",
                 st.ssid, st.password, st.code);
    } else {
        snprintf(text, sizeof(text), "start failed: %s", esp_err_to_name(err));
    }
    lv_label_set_text(s_info, text);

    s_status_line = lv_label_create(panel);
    lv_obj_set_width(s_status_line, 196);
    lv_obj_set_style_text_font(s_status_line, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_line, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_status_line, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    add_battery(s_scr);
    s_import_deadline = esp_timer_get_time() / 1000 + IMPORT_TIMEOUT_MS;
    s_import_timer = lv_timer_create(import_tick, 250, NULL);
    lv_screen_load(s_scr);
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
    case PAGE_IMPORT:   page_import_build();             break;
    case PAGE_EGG:      page_egg_build();                break;
    }
}

// ---------- 按键分发(运行于 button 组件任务,操作 LVGL 必须加锁) ----------

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

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
                    goto_page(PAGE_IMPORT);
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

    case PAGE_IMPORT:
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_LONG)) {
            goto_page(PAGE_LIST);   // teardown 中 meta_net_stop()
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

    // 让 factory(启动器自身)成为永久有效的回滚目标;非待验证态返回错误属正常。
    const esp_err_t mv = meta_store_mark_factory_valid();
    if (mv != ESP_OK && mv != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "factory 有效标记返回 %s", esp_err_to_name(mv));
    }

    meta_store_scan(s_slots);

    if (bsp_lvgl_lock(1000)) {
        page_list_build();
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "就绪:slot0=%d slot1=%d slot2=%d", s_slots[0].state, s_slots[1].state, s_slots[2].state);
}
