#include "fap_screenshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "bsp_display.h"
#include "lvgl.h"
#include <string.h>
#define TAG "fap_screenshot"
#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  320
#define SCREEN_BYTES   (SCREEN_WIDTH * SCREEN_HEIGHT * 2)
static uint8_t s_buf[SCREEN_BYTES] __attribute__((aligned(64)));
static lv_draw_buf_t s_draw_buf;
static char s_win[18];
static int  s_win_len = 0;
static void screenshot_task(void *arg)
{
    usb_serial_jtag_config_t cfg = { .rx_buffer_size = 256, .tx_buffer_size = 1024 };
    ESP_ERROR_CHECK(usb_serial_jtag_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    lv_draw_buf_init(&s_draw_buf, SCREEN_WIDTH, SCREEN_HEIGHT,
                     LV_COLOR_FORMAT_RGB565, SCREEN_WIDTH * 2, s_buf, sizeof(s_buf));
    ESP_LOGI(TAG, "ready");
    while (1) {
        if (!usb_serial_jtag_is_installed()) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        char b;
        if (usb_serial_jtag_read_bytes(&b, 1, pdMS_TO_TICKS(10)) != 1) continue;
        if (b == '\n' || b == '\r') { s_win_len = 0; continue; }
        memmove(s_win, s_win + 1, sizeof(s_win) - 1);
        s_win[sizeof(s_win) - 1] = b;
        s_win_len++;
        if (s_win_len >= 17 && strcmp(s_win, "FAP_SCREENSHOT_V1") == 0) {
            ESP_LOGI(TAG, "screenshot");
            bsp_lvgl_lock(portMAX_DELAY);
            lv_res_t r = lv_snapshot_take_to_draw_buf(NULL, &s_draw_buf);
            bsp_lvgl_unlock();
            if (r != LV_RES_OK) { ESP_LOGE(TAG, "snapshot fail"); s_win_len = 0; continue; }
            esp_log_level_set("*", ESP_LOG_NONE);
            char hdr[80];
            int hl = snprintf(hdr, sizeof(hdr), "FAP_SCREENSHOT_V1 %d %d RGB565LE %d\r\n",
                              SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_BYTES);
            size_t sent = 0;
            bool ok = true;
            while (sent < (size_t)hl && ok) {
                size_t c = hl - sent; if (c > 512) c = 512;
                size_t w;
                ok = (usb_serial_jtag_write_bytes(hdr + sent, c, pdMS_TO_TICKS(1000)) == ESP_OK);
                sent += w;
            }
            if (ok) {
                uint8_t *p = s_buf; size_t rem = SCREEN_BYTES;
                while (rem > 0 && ok) {
                    size_t c = rem; if (c > 512) c = 512;
                    size_t w;
                    ok = (usb_serial_jtag_write_bytes(p, c, pdMS_TO_TICKS(1000)) == ESP_OK);
                    p += c; rem -= c;
                }
            }
            esp_log_level_set(NULL, ESP_LOG_INFO);
            ESP_LOGI(TAG, ok ? "sent OK" : "send fail");
            s_win_len = 0;
        }
    }
}
esp_err_t fap_screenshot_init(void)
{
    return xTaskCreate(screenshot_task, "screenshot", 8192, NULL, 3, NULL) == NULL ? ESP_FAIL : ESP_OK;
}