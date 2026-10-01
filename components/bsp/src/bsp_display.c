// components/bsp/src/bsp_display.c
// 移植自 trae_card/components/platform/platform_esp32/src/disp_st7789.c
#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_disp";

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool                      s_bl_ready;

// ---------------------------------------------------------------------------
// 深睡唤醒恢复:槽位固件(如 tianshang)无操作进 deep sleep 前会 gpio_hold_en()
// 锁住 LCD/背光引脚并 gpio_deep_sleep_hold_en()。hold 可跨复位保留,唤醒后若
// 不先解除,SPI/LEDC 重新接管引脚时仍被锁在休眠电平上 —— 背光照常点亮(LEDC
// 单独一路),SPI 上的初始化命令却全部无效,表现为"有背光、界面全黑"。
// 本次唤醒若由 bootloader 钩子续期 otadata 后直接引导回槽位固件,本函数不会被
// 调用;它兜住的是仍回退到启动器的路径(子固件未适配、或续期条件不满足)。
// ---------------------------------------------------------------------------
static const gpio_num_t s_deep_sleep_pins[] = {
    BSP_LCD_CS, BSP_LCD_SCLK, BSP_LCD_MOSI, BSP_LCD_DC, BSP_LCD_BL,
};

// 与 s_deep_sleep_pins 一一对应的休眠期安全电平(CS 拉高,其余拉低)。
static const uint8_t s_deep_sleep_levels[] = {
    1, 0, 0, 0, 0,
};

static esp_err_t display_set_safe_levels(void)
{
    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < sizeof(s_deep_sleep_pins) /
                           sizeof(s_deep_sleep_pins[0]); i++) {
        gpio_num_t pin = s_deep_sleep_pins[i];
        if ((int)pin < 0) continue;
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << (unsigned)pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&cfg);
        if (e == ESP_OK) e = gpio_set_level(pin, s_deep_sleep_levels[i]);
        if (e != ESP_OK && first_error == ESP_OK) first_error = e;
    }
    return first_error;
}

// 顺序有讲究:先关全局 deep hold,再在【单引脚 hold 仍生效】时写入与休眠期
// 一致的安全电平,最后逐个解锁 —— 这样解锁瞬间引脚已经处在正确电平上,
// 不会在 SPI/LEDC 接手前产生毛刺。
static esp_err_t display_release_deep_sleep_holds(void)
{
    gpio_deep_sleep_hold_dis();
    esp_err_t first_error = display_set_safe_levels();
    for (size_t i = 0; i < sizeof(s_deep_sleep_pins) /
                           sizeof(s_deep_sleep_pins[0]); i++) {
        gpio_num_t pin = s_deep_sleep_pins[i];
        if ((int)pin < 0) continue;
        esp_err_t e = gpio_hold_dis(pin);
        if (e != ESP_OK && first_error == ESP_OK) first_error = e;
    }
    return first_error;
}

// ---------------------------------------------------------------------------
// ST7789P3 厂商专属初始化序列(porch / power / gamma)。
// 这些是【面板厂给的参考例程 TFT_init() 里的值】,不是 ST7789 通用默认值 ——
// 换面板必须找对应厂商要新的一份,照抄这份大概率显示异常。
//
// 以下三条由 esp_lcd 内置驱动完成,故此处不重复:
//   0x3A COLMOD    → esp_lcd_panel_init()
//   0x21 INVON     → esp_lcd_panel_invert_color()
//   0x29 DISPON    → esp_lcd_panel_disp_on_off()
//   0x36 MADCTL    → esp_lcd_panel_mirror()(⚠ 别再手动写 0x36,会被它覆盖)
// 0x11 SLPOUT 本也由 esp_lcd_panel_init() 下发,但深睡唤醒后必须提前到面板复位
// 之前单独发一次(停振态面板收 SWRESET 会死锁),见 bsp_display_init()。
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  cmd;
    uint8_t  data[16];
    uint8_t  len;
    uint16_t delay_ms;
} st_init_cmd_t;

static const st_init_cmd_t ST7789P3_CMDS[] = {
    {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},   // PORCTRL 帧率 porch
    {0xB7, {0x35}, 1, 0},                            // GCTRL 栅极
    {0xBB, {0x21}, 1, 0},                            // VCOMS
    {0xC0, {0x2C}, 1, 0},                            // LCMCTRL
    {0xC2, {0x01}, 1, 0},                            // VDVVRHEN
    {0xC3, {0x0B}, 1, 0},                            // VRHS
    {0xC4, {0x20}, 1, 0},                            // VDVSET
    {0xC6, {0x0F}, 1, 0},                            // FRCTRL2 60Hz 点反转
    {0xD0, {0xA7, 0xA1}, 2, 0},                      // PWCTRL1
    {0xD0, {0xA4, 0xA1}, 2, 0},                      // PWCTRL1(参考例程重发,覆盖上一条)
    {0xD6, {0xA1}, 1, 0},
    {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
            0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0},   // PVGAMCTRL 正伽马
    {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
            0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10},  // NVGAMCTRL 负伽马
};

static void backlight_init(void) {
    if (BSP_LCD_BL < 0) { ESP_LOGW(TAG, "背光引脚未接 MCU,亮度不可调"); return; }
    ledc_timer_config_t t = {
        .speed_mode      = BSP_BL_LEDC_MODE,
        .timer_num       = BSP_BL_LEDC_TIMER,
        .duty_resolution = BSP_BL_LEDC_RES,
        .freq_hz         = BSP_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t e = ledc_timer_config(&t);
    if (e != ESP_OK) { ESP_LOGE(TAG, "ledc_timer_config 失败: %s", esp_err_to_name(e)); return; }

    ledc_channel_config_t ch = {
        .gpio_num   = BSP_LCD_BL,
        .speed_mode = BSP_BL_LEDC_MODE,
        .channel    = BSP_BL_LEDC_CHANNEL,
        .timer_sel  = BSP_BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    e = ledc_channel_config(&ch);
    if (e != ESP_OK) { ESP_LOGE(TAG, "ledc_channel_config 失败: %s", esp_err_to_name(e)); return; }

    s_bl_ready = true;
    ESP_LOGI(TAG, "背光 LEDC 就绪 gpio=%d", BSP_LCD_BL);
}

esp_err_t bsp_display_init(void) {
    if (s_panel) return ESP_OK;

    // 先解除上一次深睡留下的引脚 hold,再让 SPI/LEDC 接管引脚(见上方注释)。
    const esp_err_t hold_err = display_release_deep_sleep_holds();
    if (hold_err != ESP_OK) {
        ESP_LOGE(TAG, "LCD 深睡引脚 hold 解除失败: %s(继续初始化)",
                 esp_err_to_name(hold_err));
    }

    spi_bus_config_t bus = {
        .mosi_io_num = BSP_LCD_MOSI,
        .sclk_io_num = BSP_LCD_SCLK,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_W * 80 * 2,
    };
    esp_err_t e = spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SPI 总线初始化失败 (%s) —— 检查 MOSI=GPIO%d / SCLK=GPIO%d 是否冲突",
                 esp_err_to_name(e), BSP_LCD_MOSI, BSP_LCD_SCLK);
        return e;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_LCD_CS,
        .dc_gpio_num = BSP_LCD_DC,
        .pclk_hz = BSP_LCD_PCLK_HZ,
        .spi_mode = BSP_LCD_SPI_MODE,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io_cfg, &s_io);
    if (e != ESP_OK) { ESP_LOGE(TAG, "panel_io 创建失败: %s", esp_err_to_name(e)); return e; }

    // 面板可能仍停在上次深睡的 SLPIN(0x10)停振态。本板复位脚未接 MCU
    // (BSP_LCD_RST = -1),只能走 SWRESET 软复位;而停振态的面板收 SWRESET
    // 不仅无效,还会让 SPI 命令解码状态机死锁。故在创建面板与复位之前先发
    // 0x11 SLPOUT 退眠,并延时 120ms 等内部振荡器与电荷泵稳定。
    esp_lcd_panel_io_tx_param(s_io, 0x11, NULL, 0);   // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = BSP_LCD_RST,          // -1 → SWRESET 软复位
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    e = esp_lcd_new_panel_st7789(s_io, &dev, &s_panel);
    if (e != ESP_OK) { ESP_LOGE(TAG, "面板创建失败: %s", esp_err_to_name(e)); return e; }

    esp_lcd_panel_reset(s_panel);   // rst=-1 时走 SWRESET
    esp_lcd_panel_init(s_panel);    // SLPOUT / COLMOD / RAMCTRL

    for (size_t i = 0; i < sizeof(ST7789P3_CMDS) / sizeof(ST7789P3_CMDS[0]); i++) {
        const st_init_cmd_t *c = &ST7789P3_CMDS[i];
        esp_err_t r = esp_lcd_panel_io_tx_param(s_io, c->cmd, c->data, c->len);
        if (r != ESP_OK) ESP_LOGE(TAG, "厂商初始化命令 0x%02X 失败: %s", c->cmd, esp_err_to_name(r));
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    esp_lcd_panel_invert_color(s_panel, BSP_LCD_INVERT_COLOR);   // 0x21 / 0x20
    esp_lcd_panel_mirror(s_panel, false, false);                 // 0x36 MADCTL:本板不需镜像(XY 双镜像 = 画面 180°)
    esp_lcd_panel_set_gap(s_panel, 0, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);                    // 0x29 DISPON

    backlight_init();
    ESP_LOGI(TAG, "显示就绪 %dx%d", BSP_LCD_W, BSP_LCD_H);
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_display_panel(void) { return s_panel; }

esp_lcd_panel_io_handle_t bsp_display_io(void) { return s_io; }

void bsp_display_backlight(uint8_t percent) {
    if (!s_bl_ready) return;
    if (percent > 100) percent = 100;
    uint32_t max_duty = (1u << BSP_BL_LEDC_RES) - 1u;
    uint32_t duty = (max_duty * percent) / 100u;
    ledc_set_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL);
}
