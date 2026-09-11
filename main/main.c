// main/main.c —— meta-pass 多固件启动器:槽位管理 + 导入 + 引导。
//
// 设计文档(单一权威来源):docs/assets/meta-pass-design.md
//
// 按键语义(全局统一):
//   上/下 短按   列表/详情页=移动选中项
//   确定  短按   列表=进入;详情=执行选中动作;确认页=取消
//   确定  长按   返回上一级(LONG,1.5s)
//   确定  超长按 确认页=确认危险操作(LONG2,3s;会先触发一次 LONG,确认页忽略之)
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
} page_t;

#define LIST_ITEMS   3                   // Slot 0 / Slot 1 / Import
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
static lv_obj_t *s_mascot;

// ---------- 公共小部件 ----------

// 右上角电量:读数 -1(不可用)时不画,避免显示假数字;位置在白云(188,8)下方的空闲蓝天区。
static void add_battery(lv_obj_t *parent)
{
    const int soc = bsp_battery_soc();
    char text[12];
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
    add_row(s_scr, 0, 64, "");
    add_row(s_scr, 1, 112, "");
    add_row(s_scr, 2, 160, "IMPORT FIRMWARE");
    add_battery(s_scr);
    s_mascot = ui_pixel_mascot_create(s_scr, 101, 242);
    list_refresh();
    lv_screen_load(s_scr);
}

// ---------- 页面:槽位详情 ----------

static void page_detail_build(int slot)
{
    s_detail_slot = slot;
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
                 "name: %.16s\nver:  %.16s\nsize: %lu KB\nsha: %.16s...",
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

// ---------- 页面:二次确认(危险操作统一 LONG2 确认) ----------

static void page_confirm_build(bool boot)
{
    s_scr = ui_pixel_screen_create(boot ? "BOOT?" : "DELETE?");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 60, 216, 130, UI_PAPER);
    lv_obj_t *lbl = lv_label_create(panel);
    lv_obj_set_width(lbl, 196);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_INK), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 2, 2);
    // 未签名固件无法验明来源;签名徽章本期未启用,所有第三方固件一律走本警告。
    lv_label_set_text(lbl, boot
        ? "Unsigned firmware!\nOnly boot if you trust\nthe source.\n\nOK LONG2 = boot\nOK click = cancel"
        : "Erase this slot?\nThis cannot be undone.\n\nOK LONG2 = delete\nOK click = cancel");
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
    case PAGE_CONFIRM_BOOT: page_confirm_build(true);    break;
    case PAGE_CONFIRM_DEL:  page_confirm_build(false);   break;
    case PAGE_IMPORT:   page_import_build();             break;
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
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP)   s_sel = (s_sel + DETAIL_ITEMS - 1) % DETAIL_ITEMS;
            if (btn == BSP_BTN_DOWN) s_sel = (s_sel + 1) % DETAIL_ITEMS;
            if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                rows_refresh(DETAIL_ITEMS, s_sel);
            } else if (btn == BSP_BTN_OK) {
                if (s_sel == 0 && meta_slot_bootable(s)) {          // BOOT
                    goto_page(PAGE_CONFIRM_BOOT);
                } else if (s_sel == 1 && s->state != META_SLOT_EMPTY) { // DELETE
                    goto_page(PAGE_CONFIRM_DEL);
                } else if (s_sel == 2) {                            // BACK
                    goto_page(PAGE_LIST);
                }
            }
        } else if (btn == BSP_BTN_OK && (ev == BSP_BTN_LONG || ev == BSP_BTN_LONG2)) {
            goto_page(PAGE_LIST);
        }
        break;
    }

    case PAGE_CONFIRM_BOOT:
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {   // 取消
            goto_page(PAGE_DETAIL);
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG2) {
            // 确认启动:设置启动分区并重启,不返回。
            if (meta_store_boot_slot(s_detail_slot) == ESP_OK) {
                esp_restart();
            }
            goto_page(PAGE_DETAIL);   // 设置失败(如分区损坏)则回详情页
        }
        // LONG 在本页被有意忽略:LONG2 触发前会先到来一次 LONG,不能让它误触发"返回"。
        break;

    case PAGE_CONFIRM_DEL:
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {   // 取消
            goto_page(PAGE_DETAIL);
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG2) {
            meta_store_erase_slot(s_detail_slot);
            meta_slot_clear(&s_slots[s_detail_slot]);
            goto_page(PAGE_LIST);
        }
        break;

    case PAGE_IMPORT:
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_LONG || ev == BSP_BTN_LONG2)) {
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
    ESP_LOGI(TAG, "就绪:slot0=%d slot1=%d", s_slots[0].state, s_slots[1].state);
}
